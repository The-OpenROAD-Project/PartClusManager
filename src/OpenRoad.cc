// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "opendb/db.h"
#include "opendb/lefin.h"
#include "opendb/defin.h"
#include "opendb/defout.h"

#include "Machine.hh"
#include "VerilogWriter.hh"
#include "StaMain.hh"

#include "db_sta/dbSta.hh"
#include "db_sta/MakeDbSta.hh"

#include "resizer/MakeResizer.hh"

// All this to find flute init files.
#include "flute.h"
#include <fstream>
#include <string>
#include <unistd.h> // getcwd
#include "StringUtil.hh"

#include "dbReadVerilog.hh"
#include "openroad/OpenRoad.hh"
#include "openroad/InitOpenRoad.hh"

namespace sta {
extern const char *openroad_tcl_inits[];
}

// Swig uses C linkage for init functions.
extern "C" {
extern int Openroad_Init(Tcl_Interp *interp);
extern int Opendbtcl_Init(Tcl_Interp *interp);
extern int Replace_Init(Tcl_Interp *interp);
}

namespace ord {

using std::string;

using sta::Resizer;
using odb::dbLib;
using sta::dbSta;
using odb::dbDatabase;
using sta::evalTclInit;

static void
initFlute(const char *prog_path);
static bool
readFluteInits(string dir);
static bool
fileExists(const string &filename);

OpenRoad *OpenRoad::openroad_ = nullptr;

OpenRoad::OpenRoad()
{
  openroad_ = this;
}

OpenRoad::~OpenRoad()
{
  deleteDbVerilogNetwork(verilog_network_);
  deleteDbSta(sta_);
  deleteResizer(resizer_);
  odb::dbDatabase::destroy(db_);
}

sta::dbNetwork *
OpenRoad::getDbNetwork()
{
  return sta_->getDbNetwork();
}

////////////////////////////////////////////////////////////////

void
initOpenRoad(Tcl_Interp *interp,
	     const char *prog_arg)
{
  OpenRoad *openroad = new OpenRoad;
  openroad->init(interp, prog_arg);
}

void
OpenRoad::init(Tcl_Interp *tcl_interp,
	       const char *prog_arg)
{
  tcl_interp_ = tcl_interp;

  // Make components.
  db_ = dbDatabase::create();
  sta_ = makeDbSta();
  verilog_network_ = makeDbVerilogNetwork();
  resizer_ = ord::makeResizer();

  // Init components.
  Openroad_Init(tcl_interp);
  // Import TCL scripts.
  evalTclInit(tcl_interp, sta::openroad_tcl_inits);

  Opendbtcl_Init(tcl_interp);
  initDbSta(this);
  initResizer(this);
  initDbVerilogNetwork(this);
  initFlute(prog_arg);

  Replace_Init(tcl_interp);
}

////////////////////////////////////////////////////////////////

// Flute reads look up tables from local files. gag me.
static void
initFlute(const char *prog_path)
{
  string prog_dir = prog_path;
  // Look up one directory level from /build/src.
  auto last_slash = prog_dir.find_last_of("/");
  if (last_slash != string::npos) {
    prog_dir.erase(last_slash);
    last_slash = prog_dir.find_last_of("/");
    if (last_slash != string::npos) {
      prog_dir.erase(last_slash);
      last_slash = prog_dir.find_last_of("/");
      if (last_slash != string::npos) {
	prog_dir.erase(last_slash);
	if (readFluteInits(prog_dir))
	  return;
      }
    }
  }
  // try ./etc
  prog_dir = ".";
  if (readFluteInits(prog_dir))
    return;

  // try ../etc
  prog_dir = "..";
  if (readFluteInits(prog_dir))
    return;

  // try ../../etc
  prog_dir = "../..";
  if (readFluteInits(prog_dir))
    return;

  printf("Error: could not find FluteLUT files POWV9.dat and POST9.dat.\n");
  exit(EXIT_FAILURE);
}

static bool
readFluteInits(string dir)
{
  //  printf("flute try %s\n", dir.c_str());
  string etc;
  sta::stringPrint(etc, "%s/etc", dir.c_str());
  string flute_path1;
  string flute_path2;
  sta::stringPrint(flute_path1, "%s/%s", etc.c_str(), FLUTE_POWVFILE);
  sta::stringPrint(flute_path2, "%s/%s", etc.c_str(), FLUTE_POSTFILE);
  if (fileExists(flute_path1) && fileExists(flute_path2)) {
    char *cwd = getcwd(NULL, 0);
    chdir(etc.c_str());
    Flute::readLUT();
    chdir(cwd);
    free(cwd);
    return true;
  }
  else
    return false;
}

// c++17 std::filesystem::exists
static bool
fileExists(const string &filename)
{
  std::ifstream stream(filename.c_str());
  return stream.good();
}

////////////////////////////////////////////////////////////////

void
OpenRoad::readLef(const char *filename,
		  const char *lib_name,
		  bool make_tech,
		  bool make_library)
{
  odb::lefin lef_reader(db_, false);
  if (make_tech && make_library) {
    dbLib *lib = lef_reader.createTechAndLib(lib_name, filename);
    if (lib)
      sta_->readLefAfter(lib);
  }
  else if (make_tech)
    lef_reader.createTech(filename);
  else if (make_library) {
    dbLib *lib = lef_reader.createLib(lib_name, filename);
    if (lib)
      sta_->readLefAfter(lib);
  }
}

void
OpenRoad::readDef(const char *filename)
{
  odb::defin def_reader(db_);
  std::vector<odb::dbLib *> search_libs;
  for (odb::dbLib *lib : db_->getLibs())
    search_libs.push_back(lib);
  def_reader.createChip(search_libs, filename);
  sta_->readDefAfter();
}

void
OpenRoad::writeDef(const char *filename)
{
  odb::dbChip *chip = db_->getChip();
  if (chip) {
    odb::dbBlock *block = chip->getBlock();
    if (block) {
      odb::defout def_writer;
      def_writer.writeBlock(block, filename);
    }
  }
}

void
OpenRoad::readDb(const char *filename)
{
  FILE *stream = fopen(filename, "r");
  if (stream) {
    db_->read(stream);
    sta_->readDbAfter();
    fclose(stream);
  }
}

void
OpenRoad::writeDb(const char *filename)
{
  FILE *stream = fopen(filename, "w");
  if (stream) {
    db_->write(stream);
    fclose(stream);
  }
}

void
OpenRoad::readVerilog(const char *filename)
{
  dbReadVerilog(filename, verilog_network_);
}

void
OpenRoad::linkDesign(const char *design_name)

{
  dbLinkDesign(design_name, verilog_network_, db_);
  sta_->readDbAfter();
}

void
OpenRoad::writeVerilog(const char *filename,
		       bool sort)
{
  sta::writeVerilog(filename, sort, sta_->network());
}

} // namespace
