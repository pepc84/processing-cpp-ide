// =============================================================================
// IDE.cpp  --  cpp-dev IDE
// A Processing-style creative coding IDE built with the Processing.h API.
// =============================================================================

#include "Processing.h"
#include "Platform.h"
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <regex>
#include <set>
#include <cstdio>
#include <thread>
#include <mutex>
#include <atomic>

// On Linux, guard POSIX-only headers
#ifdef PLAT_LINUX
#  include <dirent.h>
#  include <sys/stat.h>
#  include <errno.h>
#endif

namespace Processing {

// =============================================================================
// LAYOUT CONSTANTS
// =============================================================================

static const int MENUBAR_H     = 26;
static const int TOOLBAR_H     = 40;
static const int STATUS_H      = 20;
static const int GUTTER_W      = 56;
static const int TAB_H         = 22;

static int  CONSOLE_H          = 170;
static const int CONSOLE_H_MIN = 60;
static const int CONSOLE_H_MAX = 600;

static int  SIDEBAR_W          = 200;
static const int SIDEBAR_W_MIN = 120;
static const int SIDEBAR_W_MAX = 400;

static int  TERM_SIDE_W           = 340;
static const int TERM_SIDE_W_MIN  = 180;
static const int TERM_SIDE_W_MAX  = 700;

enum class TermPos { Bottom, Right };
static TermPos terminalPos = TermPos::Bottom;

static bool sidebarVisible = true;

// Font sizes (adjustable with Ctrl+= / Ctrl+-)
static float FS  = 14.0f;   // editor
static float FSS = 13.0f;   // console / toolbar
static float FST = 12.0f;   // menus / status

// Derived layout helpers
static int   sbW()         { return sidebarVisible ? SIDEBAR_W : 0; }
static float lineH()       { return FS * 1.6f; }
static int   editorX()     { return sbW() + GUTTER_W; }
static int   editorFullW() { return (terminalPos == TermPos::Right) ? width - sbW() - TERM_SIDE_W : width - sbW(); }
static int   editorY()     { return MENUBAR_H + TOOLBAR_H; }
static int   editorH()     { return (terminalPos == TermPos::Bottom) ? height - editorY() - STATUS_H - CONSOLE_H : height - editorY() - STATUS_H; }
static int   statusY()     { return editorY() + editorH(); }
static int   consoleY()    { return (terminalPos == TermPos::Bottom) ? statusY() + STATUS_H : editorY(); }
static int   consoleX()    { return (terminalPos == TermPos::Right)  ? width - TERM_SIDE_W : 0; }
static int   consoleW()    { return (terminalPos == TermPos::Right)  ? TERM_SIDE_W : width; }
static int   visLines()    { return std::max(1, (int)(editorH() / lineH())); }

// =============================================================================
// RESIZE DRAG STATE
// =============================================================================

static bool consoleResizing       = false;
static int  consoleResizeAnchorY  = 0;
static int  consoleResizeAnchorH  = 0;

static bool sidebarResizing       = false;
static int  sidebarResizeAnchorX  = 0;
static int  sidebarResizeAnchorW  = 0;

static bool termSideResizing      = false;
static int  termSideAnchorX       = 0;
static int  termSideAnchorW       = 0;

// =============================================================================
// FILE TREE (SIDEBAR)
// =============================================================================

struct FTEntry { std::string name; bool isDir; bool expanded; int depth; };
static std::vector<FTEntry> ftEntries;
static int   ftScroll = 0;
static std::string ftRoot = "files";

// List a directory -- returns {name, isDir} pairs
static std::vector<std::pair<std::string,bool>> listDir(const std::string& path) {
    std::vector<std::pair<std::string,bool>> out;
#ifdef PLAT_WINDOWS
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((path + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::string n = fd.cFileName;
        if (n == "." || n == "..") continue;
        if (!n.empty() && n[0] == '.') continue;
        out.push_back({ n, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 });
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(path.c_str());
    if (!d) return out;
    struct dirent* e;
    while ((e = readdir(d))) {
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        if (!n.empty() && n[0] == '.') continue;
        struct stat st;
        stat((path + "/" + n).c_str(), &st);
        out.push_back({ n, S_ISDIR(st.st_mode) != 0 });
    }
    closedir(d);
#endif
    return out;
}

static void populateTree() {
    ftEntries.clear();
    auto walk = [&](const std::string& path, int depth, auto& self) -> void {
        if (depth > 2) return;
        auto entries = listDir(path);
        std::vector<std::string> dirs, files;
        for (auto& [name, isDir] : entries) {
            if (isDir) dirs.push_back(name);
            else       files.push_back(name);
        }
        std::sort(dirs.begin(),  dirs.end());
        std::sort(files.begin(), files.end());
        for (auto& d : dirs) {
            bool exp = false;
            for (auto& e : ftEntries) if (e.name == d && e.isDir) { exp = e.expanded; break; }
            ftEntries.push_back({ d, true, exp, depth });
            if (exp) self(path + "/" + d, depth + 1, self);
        }
        for (auto& f : files)
            ftEntries.push_back({ f, false, false, depth });
    };
    walk(ftRoot, 0, walk);
}

// =============================================================================
// EDITOR STATE
// =============================================================================

// Forward declarations of IDE event callbacks (defined later in this file)
static void _fwd_keyPressed();
static void _fwd_keyTyped();
static void _fwd_mousePressed();
static void _fwd_mouseDragged();
static void _fwd_mouseReleased();
static void _fwd_mouseWheel(int);

// IDE event wiring -- called once before the main loop starts
static void ideWireCallbacks() {
    _onKeyPressed    = _fwd_keyPressed;
    _onKeyTyped      = _fwd_keyTyped;
    _onMousePressed  = _fwd_mousePressed;
    _onMouseDragged  = _fwd_mouseDragged;
    _onMouseReleased = _fwd_mouseReleased;
    _onMouseWheel    = _fwd_mouseWheel;
    // keyReleased, mouseClicked, mouseMoved, windowMoved, windowResized
    // not implemented by IDE -- _on* pointers stay nullptr
}

static std::vector<std::string> code = {
    "// run once",
    "",
    "void setup() {",
    "  size(640, 360);",
    "}",
    "",
    "// loops forever",
    "void draw() {",
    "  background(102);",
    "  fill(255);",
    "  ellipse(mouseX, mouseY, 40, 40);",
    "}"
};

static int  curLine   = 0;
static int  curCol    = 0;
static int  selLine   = -1;
static int  selCol    = -1;
static int  scrollTop = 0;
static bool sbDragging = false;
static float sbDragStartY = 0;
static int   sbDragStartScroll = 0;

// =============================================================================
// TERMINAL TABS
// =============================================================================

struct Terminal {
    std::string              name;
    std::vector<std::string> lines;
    int                      scroll   = 0;
    bool                     hasError = false;
};

static std::vector<Terminal> terminals = { { "Output", {}, 0, false } };
static int activeTab       = 0;
static int consoleSelLine  = -1;

// Convenience refs to tab 0 (build/run output)
static std::vector<std::string>& outLines  = terminals[0].lines;
static int&                      outScroll = terminals[0].scroll;
static bool&                     hasError  = terminals[0].hasError;

// =============================================================================
// SKETCH / BUILD STATE
// =============================================================================

static bool        modified    = false;
static std::string currentFile = "";
static std::string sketchBin   = "SketchApp";

static std::string getDefaultBuildFlags() {
#ifdef __APPLE__
    auto brewPrefix = [](const char* pkg) {
        char buf[256] = {};
        FILE* p = popen((std::string("brew --prefix ") + pkg + " 2>/dev/null").c_str(), "r");
        if (!p) return std::string();
        if (fgets(buf, sizeof(buf), p)) {}
        pclose(p);
        std::string s(buf);
        while (!s.empty() && (s.back()=='\n'||s.back()=='\r'||s.back()==' ')) s.pop_back();
        return s;
    };
    std::string glew = brewPrefix("glew"), glfw = brewPrefix("glfw"), flags;
    if (!glew.empty()) flags += "-I" + glew + "/include -L" + glew + "/lib ";
    if (!glfw.empty()) flags += "-I" + glfw + "/include -L" + glfw + "/lib ";
    flags += "-lglfw -lGLEW -framework OpenGL";
    return flags;
#elif defined(_WIN32)
    return "-lglfw3 -lglew32 -lopengl32 -lglu32 -lcomdlg32 -lshell32 -lole32 -luuid -mwindows -pthread -D_USE_MATH_DEFINES";
#else
    return "-lglfw -lGLEW -lGL -lGLU -lm -pthread";
#endif
}
static std::string buildFlags = [](){
    std::string f = getDefaultBuildFlags();
    // Enable stb_image if header is present in src/
    FILE* test = fopen("src/stb_image.h", "r");
    if (test) { fclose(test); f += " -DPROCESSING_HAS_STB_IMAGE"; }
    return f;
}();

// Sketch process (pipe capture)
static plat_proc_t       sketchProc    = plat_proc_invalid();
static std::thread       sketchThread;
static std::thread       buildThread;
static std::mutex        outMutex;
static std::atomic<bool> sketchRunning { false };
static std::atomic<bool> isBuilding    { false };   // true while g++ is running
static std::atomic<bool> buildDone     { false };   // flips to true when build finishes
static std::atomic<bool> buildSucceeded{ false };   // set by build thread on success
static std::atomic<float> buildProgress{ 0.f };    // 0..1 estimated progress for the bar
static const int         WRAP_COLS = 120;

// =============================================================================
// UNDO / REDO
// =============================================================================

using Snapshot = std::pair<std::vector<std::string>, std::pair<int,int>>;
static std::vector<Snapshot> undoStack;
static std::vector<Snapshot> redoStack;

static void pushUndo() {
    undoStack.push_back({ code, { curLine, curCol } });
    if (undoStack.size() > 200) undoStack.erase(undoStack.begin());
    redoStack.clear();
    modified = true;
}

// =============================================================================

// -- Vim tab panels --------------------------------------------------------
// The "Vim" tab in the console area shows two sub-panels:
//   "Help"   -- a read-only cheat sheet of every supported vim operation

// =============================================================================
// MENU STATE
// =============================================================================

enum class Menu { None, File, Edit, Sketch, Tools, Libraries };
static Menu openMenu = Menu::None;

// =============================================================================
// LIBRARY MANAGER
// =============================================================================

struct Library {
    std::string name, desc, pkg, header, installCmd, linkFlag;
    bool installed = false;
};

static std::vector<Library> libraries = {
    { "libserialport",  "Cross-platform serial (sigrok)",    "libserialport-dev",     "#include <libserialport.h>",             "", "-lserialport"  },
    { "Boost (Asio)",   "Boost.Asio serial_port",            "boost",                 "#include <boost/asio/serial_port.hpp>",  "", "-lboost_system"},
    { "Eigen",          "Linear algebra: matrices/vectors",  "libeigen3-dev",         "#include <Eigen/Dense>",                 "", ""              },
    { "glm",            "OpenGL math (vec3, mat4)",          "libglm-dev",            "#include <glm/glm.hpp>",                 "", ""              },
    { "Box2D",          "2D rigid body physics",             "libbox2d-dev",          "#include <box2d/box2d.h>",               "", "-lbox2d"       },
    { "FFTW3",          "Fast Fourier Transform",            "libfftw3-dev",          "#include <fftw3.h>",                     "", "-lfftw3"       },
    { "OpenCV",         "Computer vision",                   "libopencv-dev",         "#include <opencv2/opencv.hpp>",          "", "-lopencv_core" },
    { "PortAudio",      "Low-latency audio I/O",             "portaudio19-dev",       "#include <portaudio.h>",                 "", "-lportaudio"   },
    { "libcurl",        "HTTP/HTTPS requests",               "libcurl4-openssl-dev",  "#include <curl/curl.h>",                 "", "-lcurl"        },
    { "SQLite3",        "Embedded SQL database",             "libsqlite3-dev",        "#include <sqlite3.h>",                   "", "-lsqlite3"     },
    { "nlohmann/json",  "Header-only C++ JSON",              "",                      "#include \"json.hpp\"",
      "curl -sL https://github.com/nlohmann/json/releases/latest/download/json.hpp -o src/json.hpp", "" },
    { "stb_image",      "Header-only image loader",          "",                      "#include \"stb_image.h\"",
      "curl -sL https://raw.githubusercontent.com/nothings/stb/master/stb_image.h -o src/stb_image.h", "" },
};

static bool showLibMgr    = false;
static bool showExamples  = false;   // Examples browser overlay
static int  exScroll      = 0;       // scroll position in examples list
static int  exHover       = -1;      // which row the mouse is over
static bool debuggerMode      = false; // Example Debugger: auto-cycle mode
static int  debuggerIndex     = 0;     // current example index in debugger
static bool debuggerAdvance   = false; // signal main thread to advance+compile

struct ExampleEntry {
    std::string name;   // filename only (no folder prefix)
    std::string path;   // full relative path
    std::string desc;   // one-line description
    std::string folder; // folder name, "" for root
};
// A row in the examples list is either a FOLDER header or a FILE entry
struct ExRow {
    bool   isFolder;
    std::string label;    // folder name or file display name
    std::string path;     // empty for folders
    std::string desc;
    std::string folder;
};
static std::vector<ExampleEntry> examplesList;
static std::vector<ExRow>        exRows;          // display rows (built from examplesList)
static std::set<std::string>     exOpenFolders;   // which folders are expanded

// Read one-line description from the first // comment in a .cpp file
static std::string readSketchDesc(const std::string& path) {
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        // Skip blank lines and === section headers
        if (line.size()>3 && line.substr(0,3)=="// "
                && line.find("====") == std::string::npos
                && line.find("----") == std::string::npos) {
            std::string d = line.substr(3);
            if (d.size()>64) d = d.substr(0,61)+"...";
            return d;
        }
    }
    return "";
}

// Recursively scan a directory for .cpp files, adding them to examplesList.
// folder      = the directory to scan  (e.g. "examples/camera")
// displayRoot = prefix shown in the name column (e.g. "camera/")
static void scanExamplesDir(const std::string& folder,
                             const std::string& displayRoot) {
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    // First pass: .cpp files in this folder
    HANDLE h = FindFirstFileA((folder + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::string> subdirs;
    do {
        std::string n = fd.cFileName;
        if (n == "." || n == "..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            subdirs.push_back(n);
        } else if (n.size()>4 && n.substr(n.size()-4)==".cpp") {
            ExampleEntry e;
            e.name = n;
            e.path = folder + "\\" + n;
            e.desc = readSketchDesc(e.path);
            e.folder = displayRoot.empty() ? "" : displayRoot.substr(0, displayRoot.size()-1);
            examplesList.push_back(e);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    // Second pass: recurse into subdirectories
    for (auto& sub : subdirs)
        scanExamplesDir(folder + "\\" + sub, displayRoot + sub + "/");
#else
    DIR* d = opendir(folder.c_str());
    if (!d) return;
    std::vector<std::string> subdirs;
    struct dirent* ent;
    while ((ent = readdir(d))) {
        std::string n = ent->d_name;
        if (n == "." || n == "..") continue;
        std::string full = folder + "/" + n;
        struct stat st;
        if (stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            subdirs.push_back(n);
        } else if (n.size()>4 && n.substr(n.size()-4)==".cpp") {
            ExampleEntry e;
            e.name = n;
            e.path = full;
            e.desc = readSketchDesc(e.path);
            e.folder = displayRoot.empty() ? "" : displayRoot.substr(0, displayRoot.size()-1);
            examplesList.push_back(e);
        }
    }
    closedir(d);
    for (auto& sub : subdirs)
        scanExamplesDir(folder + "/" + sub, displayRoot + sub + "/");
#endif
}

// Scan examples/ directory and all its subdirectories
static void buildExRows() {
    exRows.clear();
    // Collect unique folder names (in order)
    std::vector<std::string> folders;
    for (auto& e : examplesList) {
        if (!e.folder.empty()) {
            if (std::find(folders.begin(), folders.end(), e.folder) == folders.end())
                folders.push_back(e.folder);
        }
    }
    // Root-level files first
    for (auto& e : examplesList) {
        if (e.folder.empty()) {
            ExRow r; r.isFolder=false; r.label=e.name; r.path=e.path; r.desc=e.desc; r.folder="";
            exRows.push_back(r);
        }
    }
    // Then each folder as a header + its files (if expanded)
    std::sort(folders.begin(), folders.end());
    for (auto& f : folders) {
        ExRow hdr; hdr.isFolder=true; hdr.label=f; hdr.folder=f;
        exRows.push_back(hdr);
        if (exOpenFolders.count(f)) {
            for (auto& e : examplesList) {
                if (e.folder == f) {
                    ExRow r; r.isFolder=false; r.label=e.name; r.path=e.path; r.desc=e.desc; r.folder=f;
                    exRows.push_back(r);
                }
            }
        }
    }
}

static void refreshExamples() {
    examplesList.clear();
    scanExamplesDir("examples", "");
    std::sort(examplesList.begin(), examplesList.end(),
              [](const ExampleEntry& a, const ExampleEntry& b){
                  if (a.folder != b.folder) return a.folder < b.folder;
                  return a.name < b.name;
              });
    buildExRows();
}
static int  libScroll     = 0;
static int  installingLib = -1;
static std::string libStatus = "";

// Package manager helpers (Linux only)
#ifdef PLAT_LINUX
enum class PkgMgr { Unknown, Apt, Pacman, Dnf };
static PkgMgr detectPkgMgr() {
    if (system("command -v pacman  >/dev/null 2>&1") == 0) return PkgMgr::Pacman;
    if (system("command -v apt-get >/dev/null 2>&1") == 0) return PkgMgr::Apt;
    if (system("command -v dnf     >/dev/null 2>&1") == 0) return PkgMgr::Dnf;
    return PkgMgr::Unknown;
}
struct PkgMap { std::string apt, pacman, dnf; };
static const std::vector<PkgMap> PKG_MAP = {
    { "libserialport-dev",   "libserialport",  "libserialport-devel" },
    { "boost",               "boost",          "boost-devel"         },
    { "libeigen3-dev",       "eigen",          "eigen3-devel"        },
    { "libopencv-dev",       "opencv",         "opencv-devel"        },
    { "portaudio19-dev",     "portaudio",      "portaudio-devel"     },
    { "libcurl4-openssl-dev","curl",           "libcurl-devel"       },
    { "libglm-dev",          "glm",            "glm-devel"           },
    { "libbox2d-dev",        "box2d",          "box2d-devel"         },
    { "libsqlite3-dev",      "sqlite",         "sqlite-devel"        },
    { "libfftw3-dev",        "fftw",           "fftw-devel"          },
};
static std::string resolvePkg(const std::string& apt) {
    PkgMgr pm = detectPkgMgr();
    if (pm == PkgMgr::Apt) return apt;
    for (auto& p : PKG_MAP) {
        if (p.apt == apt) {
            if (pm == PkgMgr::Pacman) return p.pacman;
            if (pm == PkgMgr::Dnf)    return p.dnf;
        }
    }
    return apt;
}
static std::string buildInstallCmd(const std::string& pkg) {
    return plat_build_install_cmd(resolvePkg(pkg), pkg);
}
static bool isPkgInstalled(const std::string& pkg) {
    std::string p = resolvePkg(pkg);
    switch (detectPkgMgr()) {
        case PkgMgr::Pacman: return system(("pacman -Q " + p + " >/dev/null 2>&1").c_str()) == 0;
        case PkgMgr::Apt:    return system(("dpkg -s "   + p + " >/dev/null 2>&1").c_str()) == 0;
        case PkgMgr::Dnf:    return system(("rpm -q "    + p + " >/dev/null 2>&1").c_str()) == 0;
        default:             return false;
    }
}
#else
static std::string buildInstallCmd(const std::string& pkg) { return plat_build_install_cmd(pkg, pkg); }
static bool isPkgInstalled(const std::string&) { return false; }
#endif

static void checkInstalled() {
    for (auto& lib : libraries) {
        if (lib.pkg.empty()) {
            {
                // Header-only: check for file in src/
                std::string h = lib.header;
                size_t a = h.find('"'), b = h.rfind('"');
                if (a != std::string::npos && b != a)
                    lib.installed = plat_file_exists("src/" + h.substr(a+1, b-a-1));
            }
        } else {
            lib.installed = isPkgInstalled(lib.pkg);
        }
    }
}

// =============================================================================
// CURSOR / SELECTION HELPERS
// =============================================================================

static void clamp() {
    curLine = std::max(0, std::min(curLine, (int)code.size()-1));
    curCol  = std::max(0, std::min(curCol,  (int)code[curLine].size()));
}

static void ensureVis() {
    if (curLine < scrollTop) scrollTop = curLine;
    if (curLine >= scrollTop + visLines()) scrollTop = curLine - visLines() + 1;
    scrollTop = std::max(0, scrollTop);
}

static bool hasSel()   { return selLine >= 0; }
static void clearSel() { selLine = -1; selCol = -1; }

static void selRange(int& l0, int& c0, int& l1, int& c1) {
    if (selLine < curLine || (selLine == curLine && selCol <= curCol))
        { l0 = selLine; c0 = selCol; l1 = curLine; c1 = curCol; }
    else
        { l0 = curLine; c0 = curCol; l1 = selLine; c1 = selCol; }
}

static std::string getSelected() {
    if (!hasSel()) return "";
    int l0, c0, l1, c1;
    selRange(l0, c0, l1, c1);
    if (l0 == l1) return code[l0].substr(c0, c1 - c0);
    std::string s = code[l0].substr(c0) + "\n";
    for (int l = l0+1; l < l1; l++) s += code[l] + "\n";
    s += code[l1].substr(0, c1);
    return s;
}

static void deleteSel() {
    if (!hasSel()) return;
    int l0, c0, l1, c1;
    selRange(l0, c0, l1, c1);
    if (l0 == l1) {
        code[l0].erase(c0, c1 - c0);
    } else {
        code[l0] = code[l0].substr(0, c0) + code[l1].substr(c1);
        code.erase(code.begin() + l0 + 1, code.begin() + l1 + 1);
    }
    curLine = l0; curCol = c0;
    clearSel();
}

// =============================================================================
// FILE OPERATIONS
// =============================================================================

static void newFile() {
    code = {
        "// run once",
        "",
        "void setup() {",
        "  size(640, 360);",
        "}",
        "",
        "// loops forever",
        "void draw() {",
        "  background(102);",
        "  fill(255);",
        "  ellipse(mouseX, mouseY, 40, 40);",
        "}"
    };
    curLine = curCol = scrollTop = 0;
    clearSel();
    undoStack.clear();
    redoStack.clear();
    currentFile = "";
    sketchBin   = "SketchApp";
    modified    = false;
    outLines.push_back("New sketch.");
}

static void saveFile(const std::string& path) {
    std::string p = path;
    if (p.size() < 4 || p.substr(p.size()-4) != ".cpp") p += ".cpp";
    std::ofstream f(p);
    if (!f) { outLines.push_back("ERROR: cannot save " + p); return; }
    for (auto& l : code) f << l << "\n";
    currentFile = p;
    modified    = false;
    outLines.push_back("Saved: " + p);
    outScroll = std::max(0, (int)outLines.size() - 8);
}

static void openFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) { outLines.push_back("ERROR: cannot open " + path); return; }
    code.clear();
    std::string l;
    while (std::getline(f, l)) code.push_back(l);
    if (code.empty()) code.push_back("");
    curLine = curCol = scrollTop = 0;
    clearSel();
    undoStack.clear();
    redoStack.clear();
    currentFile = path;
    modified    = false;
    outLines.push_back("Opened: " + path);
}

// Export Application state
static bool      showExportDlg = false;
static bool      exportWin64   = true;
static bool      exportLinux64 = false;
static bool      exportMac     = false;
static std::string exportStatus = "";
static bool      exportRunning  = false;
static std::thread exportThread;

// File picker state (used as fallback when system dialog unavailable)
static bool fpShow  = false;
static bool fpSave  = false;
static std::string fpInput = "";

static void doOpen() {
    std::string path = plat_file_dialog(false, currentFile);
    if (!path.empty()) openFile(path);
    // If empty the user cancelled -- do nothing
}

static void doSaveAs(const std::string& def = "") {
    std::string start = def.empty() ? currentFile : def;
    std::string path = plat_file_dialog(true, start);
    if (!path.empty()) {
        // Ensure .cpp extension
        if (path.size() < 4 || path.substr(path.size()-4) != ".cpp")
            path += ".cpp";
        saveFile(path);
        windowTitle("cpp-dev IDE -- " + path);
    }
    // If dialog returns empty the user cancelled -- do nothing
}

static void doSave() {
    if (currentFile.empty()) {
        // No file yet -- always open native Save As dialog
        doSaveAs();
    } else {
        saveFile(currentFile);
        windowTitle("cpp-dev IDE -- " + currentFile);
    }
}

static std::vector<std::string> listSketches() {
    return plat_list_sketches();
}

// =============================================================================
// SKETCH SANITIZER  (strips BOM / smart quotes before writing Sketch_run.cpp)
// =============================================================================

// ---------------------------------------------------------------------------
// Java -> C++ keyword translation
// Replaces whole-word occurrences of Java-only keywords with their C++
// equivalents. Applied to every line before writing to Sketch_run.cpp.
// ---------------------------------------------------------------------------
static std::string javaToC(const std::string& line) {
    // Table of whole-word replacements: { java_token, cpp_token }
    static const std::pair<std::string,std::string> REPLACEMENTS[] = {
        // Java primitive types that differ
        { "boolean",   "bool"        },
        { "Integer",   "int"         },
        { "Float",     "float"       },
        { "Double",    "double"      },
        { "Long",      "long"        },
        { "Byte",      "char"        },
        { "Character", "char"        },
        { "String",    "std::string" },
        // Java literals
        { "true",      "true"        }, // same but ensure no mangling
        { "false",     "false"       },
        { "null",      "nullptr"     },
        // Java cast syntax  float(x) -> (float)(x)  int(x) -> (int)(x)
        // Handled separately below as they need special treatment.
        // Java array declaration  int[] x  ->  int x[]  (already valid C++)
        // Java new array  new float[n]  ->  just removed (static arrays used)
        // Trailing semicolons on class-level booleans etc are already fine.
    };

    // Skip lines that are pure comments or preprocessor directives
    std::string trimmed = line;
    while (!trimmed.empty() && (trimmed[0]==' '||trimmed[0]=='\t')) trimmed=trimmed.substr(1);
    bool isComment = (trimmed.size()>=2 && trimmed[0]=='/' && (trimmed[1]=='/'||trimmed[1]=='*'));
    bool isPreproc = (!trimmed.empty() && trimmed[0]=='#');
    if (isComment || isPreproc) return line;

    std::string out = line;

    // Whole-word replacement: only replace when surrounded by non-identifier chars
    auto isIdChar = [](char c){ return isalnum((unsigned char)c) || c=='_'; };

    for (auto& [from, to] : REPLACEMENTS) {
        std::string result;
        size_t i = 0, n = out.size();
        bool inStr = false, inChar = false;
        while (i < n) {
            // Track string/char literals to avoid replacing inside them
            if (!inChar && !inStr && out[i] == '"')  { inStr  = true;  result += out[i++]; continue; }
            if (inStr) {
                if (out[i] == '\\' && i+1 < n)      { result += out[i++]; result += out[i++]; continue; }
                if (out[i] == '"')                    { inStr = false; result += out[i++]; continue; }
                result += out[i++]; continue;
            }
            if (!inStr && out[i] == '\'')            { inChar = true;  result += out[i++]; continue; }
            if (inChar) {
                if (out[i] == '\\' && i+1 < n)      { result += out[i++]; result += out[i++]; continue; }
                if (out[i] == '\'')                   { inChar = false; result += out[i++]; continue; }
                result += out[i++]; continue;
            }
            // Outside string/char: check for keyword match
            size_t flen = from.size();
            bool leftOk  = (i == 0) || !isIdChar(out[i-1]);
            bool matches = leftOk && (i + flen <= n) && out.substr(i, flen) == from;
            bool rightOk = matches && ((i+flen >= n) || !isIdChar(out[i+flen]));
            if (matches && rightOk) { result += to; i += flen; }
            else                    { result += out[i++]; }
        }
        out = result;
    }

    // Java cast syntax: float(expr) -> (float)(expr)
    // Matches: float( int( double( etc. at word boundary not preceded by identifier
    static const char* CAST_TYPES[] = { "float","int","double","long","char","bool",nullptr };
    for (int ci = 0; CAST_TYPES[ci]; ci++) {
        std::string pat = std::string(CAST_TYPES[ci]) + "(";
        std::string result;
        size_t pos = 0;
        while (pos < out.size()) {
            size_t found = out.find(pat, pos);
            if (found == std::string::npos) { result += out.substr(pos); break; }
            bool leftOk = (found==0) || !isIdChar(out[found-1]);
            if (leftOk) {
                result += out.substr(pos, found-pos);
                result += std::string("(") + CAST_TYPES[ci] + ")(";
                pos = found + pat.size();
            } else {
                result += out.substr(pos, found-pos+1);
                pos = found + 1;
            }
        }
        out = result;
    }


    // Convert (double_expr) % int to (int)(double_expr) % int
    // The % operator doesn't work on floats/doubles in C++
    {
        // Match: (expr) % varname where expr contains a dot (float result)
        std::regex modFloat(R"(\(([^)]+\.[^)]*)\)\s*%\s*(\w+))");
        out = std::regex_replace(out, modFloat, "(int)($1) % $2");
    }

    // Rename Windows-reserved identifiers that conflict with WinAPI macros.
    // e.g. `float far = ...` fails because <windows.h> defines `far` as empty macro.
    {
        auto renameReserved = [](std::string& s, const std::string& word, const std::string& repl) {
            std::string r; size_t i=0, wl=word.size();
            while (i<=s.size()) {
                if (i+wl<=s.size() && s.substr(i,wl)==word) {
                    bool prevOk=(i==0)||(!isalnum((unsigned char)s[i-1])&&s[i-1]!='_');
                    bool nextOk=(i+wl>=s.size())||(!isalnum((unsigned char)s[i+wl])&&s[i+wl]!='_');
                    if (prevOk && nextOk) { r+=repl; i+=wl; continue; }
                }
                if (i<s.size()) r+=s[i]; i++;
            }
            s=r;
        };
        // Windows macros that break variable names
        renameReserved(out, "far",   "farVal");
        renameReserved(out, "near",  "nearVal");
    }

    // Replace `int var = lerpColor(` and `int var = color(` with `color var = ...`
    // because in Processing Java, color IS int, but in C++ they differ.
    {
        // Match: (optional whitespace) int (spaces) (identifier) (spaces) = (spaces) (color|lerpColor)(
        std::regex colorAssign(R"(\bint\s+(\w+)\s*=\s*(color|lerpColor)\s*\()");
        out = std::regex_replace(out, colorAssign, "color $1 = $2(");
    }

    // Rename variables that shadow Processing API functions.
    // e.g. `float scale;` conflicts with scale(x,y) transform.
    // Only rename when the name appears as a VARIABLE (after a type keyword).
    // This renames the declaration AND all uses via whole-word replace.
    {
        static const std::vector<std::pair<std::string,std::string>> apiConflicts = {
            {"scale","_scale"},{"fill","_fill"},{"stroke","_stroke"},
            {"background","_background"},{"translate","_translate"},
            {"rotate","_rotate"},{"map","_map"},{"dist","_dist"},{"noise","_noise"},
        };
        // Match: type keyword followed by the conflicting name as a variable
        // (not followed by '(' which would make it a function call)
        static const std::string typeKw =
            R"((?:int|float|double|bool|char|long|unsigned|auto|color|PImage\*?|PVector)\s+)";
        for (auto& [word, repl] : apiConflicts) {
            // Check if this word appears as a variable declaration
            std::regex declPat(typeKw + "(" + word + R"()(?!\s*\())");
            if (std::regex_search(out, declPat)) {
                // Rename all whole-word occurrences that are NOT followed by '('
                // Strategy: rename ALL occurrences, then fix function calls back
                std::string renamed;
                size_t i = 0, wl = word.size();
                while (i <= out.size()) {
                    if (i + wl <= out.size() && out.substr(i,wl) == word) {
                        bool prevOk = (i==0)||(!isalnum((unsigned char)out[i-1])&&out[i-1]!='_');
                        bool nextOk = (i+wl>=out.size())||(!isalnum((unsigned char)out[i+wl])&&out[i+wl]!='_');
                        if (prevOk && nextOk) {
                            // Check if this is a function call (followed by '(' possibly with spaces)
                            size_t j = i + wl;
                            while (j < out.size() && out[j] == ' ') j++;
                            bool isCall = (j < out.size() && out[j] == '(');
                            if (!isCall) {
                                renamed += repl; i += wl; continue;
                            }
                        }
                    }
                    if (i < out.size()) renamed += out[i];
                    i++;
                }
                out = renamed;
            }
        }
    }

    // (color param name heuristic removed -- too many false positives)

    // No PImage/PFont auto-translation -- write explicit C++ pointer syntax.

    // Java String concatenation -> C++ fixes:
    // 1. "literal" + char/int: in Java this works; in C++ "literal" is const char*
    //    and adding a char/int does pointer arithmetic. Wrap in std::string().
    // 2. words.length() in concat: returns size_t which can't concat with string.
    //    Wrap in std::to_string().
    {
        // Fix 1: string literal + something -> std::string("literal") + something
        // Match a quoted string followed by + at end (possibly with spaces)
        std::regex strConcatRe(R"(("(?:[^"\\]|\\.)*")\s*\+)");
        out = std::regex_replace(out, strConcatRe, "std::string($1) +");

        // Fix 2: + expr.length() -> + std::to_string(expr.length())
        std::regex concatLen(R"(\+\s*(\w+)\.length\(\))");
        out = std::regex_replace(out, concatLen, "+ std::to_string($1.length())");
    }
    // .charAt(i) -> [i]  (handled below as regex)
    // .substring(a,b) -> .substr(a, b-a)
    // .toUpperCase() -> (need manual or leave -- not common in Processing sketches)
    // .equals(s) -> == s
    {
        // .charAt(i) -> [i]
        std::regex charAtRe(R"(\.charAt\((\w+)\))");
        out = std::regex_replace(out, charAtRe, "[$1]");
        // .equals("...") -> == "..."
        std::regex equalsRe(R"(\.equals\(([^)]*)\))");
        out = std::regex_replace(out, equalsRe, " == $1");
        // .substring(a, b) -> .substr(a, (b)-(a))
        // Too complex for simple regex -- skip for now
    }

    // color() ambiguity fix: color(int,int,int,floatVar) is ambiguous in C++.
    // Cast float variables in color() calls to (int) to resolve.
    // This handles: color(0, 153, 204, a) where a is float.
    {
        // Match color( followed by 3 ints and a float variable as last arg
        std::regex colorMixed(R"(\bcolor\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*([^)\d][^)]*)\))");
        out = std::regex_replace(out, colorMixed, "color($1,$2,$3,(int)($4))");
    }

    // Java/Processing hex color literals: #RRGGBB or #AARRGGBB -> color(0xFFRRGGBB)
    {
        std::string result;
        size_t pos = 0;
        while (pos < out.size()) {
            if (out[pos] == '#' && (pos == 0 || !isIdChar(out[pos-1]))) {
                // Check if followed by 6 or 8 hex digits
                size_t end = pos + 1;
                while (end < out.size() && isxdigit((unsigned char)out[end])) end++;
                size_t hexlen = end - pos - 1;
                if (hexlen == 6) {
                    result += "color(0xFF" + out.substr(pos+1, 6) + ")";
                    pos = end; continue;
                } else if (hexlen == 8) {
                    result += "color(0x" + out.substr(pos+1, 8) + ")";
                    pos = end; continue;
                }
            }
            result += out[pos++];
        }
        out = result;
    }

    return out;
}

static std::string sanitizeLine(const std::string& s) {
    std::string out;
    size_t i = 0;
    // Strip UTF-8 BOM
    if (s.size() >= 3 &&
        (unsigned char)s[0] == 0xEF &&
        (unsigned char)s[1] == 0xBB &&
        (unsigned char)s[2] == 0xBF) i = 3;

    for (; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { out += (char)c; continue; } // plain ASCII

        // Replace common typographic substitutions with ASCII
        if (c == 0xE2 && i + 2 < s.size()) {
            unsigned char b1 = (unsigned char)s[i+1];
            unsigned char b2 = (unsigned char)s[i+2];
            if (b1 == 0x80) {
                if      (b2==0x98||b2==0x99) { out += '\''; i+=2; } // curly single quote
                else if (b2==0x9C||b2==0x9D) { out += '"';  i+=2; } // curly double quote
                else if (b2==0x93||b2==0x94) { out += '-';  i+=2; } // en/em dash
                else                         {               i+=2; } // drop other
            } else { i += 2; }
        } else if (c >= 0xC0 && c <= 0xDF && i+1 < s.size()) { i += 1; }
        else if (c >= 0xF0 && i+3 < s.size())                { i += 3; }
        // else: drop stray high byte
    }
    // Apply Java->C++ keyword translation after encoding cleanup
    return javaToC(out);
}

static bool writeSketch() {
    std::ofstream f("src/Sketch_run.cpp");
    if (!f) {
        outLines.push_back("ERROR: cannot write src/Sketch_run.cpp");
        hasError = true;
        return false;
    }

    // Detect sketch mode -- same rules as Processing Java:
    //   - If the sketch defines void setup() or void draw(), it is a
    //     "structured" sketch and we wrap it in namespace Processing only.
    //   - If neither is present, it is a "static" or "top-level" sketch
    //     (like the Mandelbrot example) where all code sits outside any
    //     function. We wrap the entire body in setup() { ... } so it
    //     compiles as valid C++. draw() is left as a no-op (noLoop()
    //     should be called by the sketch if it doesn't want looping).
    bool hasSetup = false;
    bool hasDraw  = false;
    bool hasNS    = false;
    for (auto& l : code) {
        // Look for function definitions (not just mentions)
        if (l.find("void setup(") != std::string::npos) hasSetup = true;
        if (l.find("void draw(")  != std::string::npos) hasDraw  = true;
        if (l.find("namespace Processing") != std::string::npos) hasNS = true;
    }

    bool isTopLevel = !hasSetup && !hasDraw && !hasNS;

    f << "#include \"Processing.h\"\n";
    f << "namespace Processing {\n";
    // Bring in safe std names that don't conflict with Processing functions.
    // Conflicting names (map, fill, copy, set) must be used as std::map etc.
    f << "using std::vector; using std::string; using std::wstring;\n";
    f << "using std::pair; using std::make_pair; using std::tuple;\n";
    f << "using std::deque; using std::list; using std::stack; using std::queue;\n";
    f << "using std::unordered_map; using std::unordered_set;\n";
    f << "using std::sort; using std::shuffle; using std::reverse;\n";
    f << "using std::unique_ptr; using std::shared_ptr; using std::make_unique; using std::make_shared;\n";
    f << "using std::optional; using std::variant;\n";
    f << "using std::to_string; using std::stoi; using std::stof; using std::stod;\n";
    f << "using std::random_device; using std::mt19937; using std::mt19937_64;\n";
    f << "using std::uniform_int_distribution; using std::uniform_real_distribution;\n";
    f << "using std::normal_distribution;\n";
    f << "using std::cout; using std::cerr; using std::endl;\n";
    f << "using std::ifstream; using std::ofstream; using std::stringstream;\n";

    // Pre-scan: find variable names assigned color()/lerpColor() to fix their type.
    // Also propagates: if drawBand(a,b,...) is called where a,b are colorVars,
    // the corresponding params of drawBand are also treated as color.
    std::set<std::string> colorVars;
    {
        // Pass 1: direct assignments
        std::regex assignRe(R"(\b(\w+)\s*=\s*(color|lerpColor)\s*\()");
        for (auto& l : code) {
            std::smatch m; std::string sl = l;
            while (std::regex_search(sl, m, assignRe)) {
                colorVars.insert(m[1].str());
                sl = m.suffix().str();
            }
        }

        // Pass 2: propagate through function calls.
        // For each function call where some args are colorVars, find the
        // function definition and add the corresponding param names to colorVars.
        // Repeat until stable.
        bool changed = true;
        while (changed) {
            changed = false;
            // Find function definitions: retType name(params) {
            std::regex fnDefRe(R"(^\s*(?:void|int|float|bool)\s+(\w+)\s*\(([^)]*)\))");
            for (size_t li = 0; li < code.size(); li++) {
                std::smatch mDef;
                if (!std::regex_search(code[li], mDef, fnDefRe)) continue;
                std::string fnName = mDef[1].str();
                std::string params = mDef[2].str();
                // Extract param names
                std::vector<std::string> paramNames;
                std::regex pnRe(R"(\w+\s+(\w+))");
                std::string ps = params;
                std::smatch pm;
                while (std::regex_search(ps, pm, pnRe)) {
                    paramNames.push_back(pm[1].str());
                    ps = pm.suffix().str();
                }
                if (paramNames.empty()) continue;
                // Find calls to this function with colorVar args
                std::regex callRe("\\b" + fnName + "\\s*\\(([^;{]*)\\)");
                for (auto& cl : code) {
                    if (cl.size() > 500) continue; // skip very long lines to avoid regex backtracking
                    std::smatch mCall;
                    std::string sl2 = cl;
                    int _guard = 0;
                    while (_guard++ < 10 && std::regex_search(sl2, mCall, callRe)) {
                        // Split args by comma (simplified -- no nested parens)
                        std::string argStr = mCall[1].str();
                        std::vector<std::string> args;
                        std::string cur;
                        for (char ch : argStr) {
                            if (ch == ',') { args.push_back(cur); cur.clear(); }
                            else cur += ch;
                        }
                        args.push_back(cur);
                        // Check which arg positions have colorVars
                        for (size_t ai = 0; ai < args.size() && ai < paramNames.size(); ai++) {
                            std::string arg = args[ai];
                            // strip whitespace
                            arg.erase(0, arg.find_first_not_of(" \t"));
                            arg.erase(arg.find_last_not_of(" \t") + 1);
                            if (colorVars.count(arg) && !colorVars.count(paramNames[ai])) {
                                colorVars.insert(paramNames[ai]);
                                changed = true;
                            }
                        }
                        sl2 = mCall.suffix().str();
                    }
                }
            }
        }

        // Pass 3: also mark int arrays of colorVars as color arrays.
        // e.g. `int colorOrder[5] = {v, w, x, y, z}` where v..z are colorVars
        std::regex arrRe(R"(\bint\s+(\w+)\[\d+\]\s*=\s*\{([^}]*)\})");
        for (auto& l : code) {
            std::smatch m;
            std::string sl = l;
            while (std::regex_search(sl, m, arrRe)) {
                std::string argStr = m[2].str();
                bool anyColor = false;
                std::string cur;
                for (char ch : argStr + ",") {
                    if (ch == ',') {
                        cur.erase(0, cur.find_first_not_of(" \t"));
                        cur.erase(cur.find_last_not_of(" \t") + 1);
                        if (colorVars.count(cur)) anyColor = true;
                        cur.clear();
                    } else cur += ch;
                }
                if (anyColor) colorVars.insert(m[1].str());
                sl = m.suffix().str();
            }
        }
    }

    // Forward-declare all user-defined void/int/float/bool functions
    // so they can be called before their definition in the file.
    {
        std::regex fnRe(R"(^\s*(void|int|float|bool|double|color|std::string)\s+(\w+)\s*\()");
        for (auto& l : code) {
            std::smatch m;
            std::string sl = sanitizeLine(l);
            if (std::regex_search(sl, m, fnRe)) {
                std::string retType = m[1].str();
                std::string fnName = m[2].str();
                // Skip Processing built-ins
                if (fnName=="setup"||fnName=="draw"||fnName=="keyPressed"||
                    fnName=="keyReleased"||fnName=="keyTyped"||
                    fnName=="mousePressed"||fnName=="mouseReleased"||
                    fnName=="mouseClicked"||fnName=="mouseMoved"||
                    fnName=="mouseDragged"||fnName=="mouseWheel"||
                    fnName=="windowMoved"||fnName=="windowResized"||
                    fnName=="settings") continue;
                // Emit a forward declaration (just the return type and name with ...)
                // We need the full signature -- find the closing ) on this line
                size_t open = sl.find('(');
                size_t close = sl.rfind(')');
                if (open!=std::string::npos && close!=std::string::npos && close>open) {
                    std::string sig = sl.substr(open, close-open+1);
                    // Apply colorVars patch to forward decl signature too,
                    // so it matches the (patched) definition.
                    for (auto& cv : colorVars) {
                        std::regex re("\\bint\\s+(" + cv + ")\\b(\\s*\\[\\d+\\])?");
                        sig = std::regex_replace(sig, re, "color $1$2");
                    }
                    f << retType << " " << fnName << sig << ";\n";
                }
            }
        }
    }

    if (isTopLevel) {
        // Wrap the whole sketch body in setup() so top-level code compiles.
        // Call noLoop() at end so the blank draw() doesn't wipe the frame.
        // This matches Processing Java's static mode exactly.
        f << "\nvoid setup() {\n";
        for (auto& l : code) {
            std::string sl = sanitizeLine(l);
            // Fix int->color for variables known to hold color values
            for (auto& cv : colorVars) {
                // Replace `int varname` and `int varname[N]` with color equivalents
                std::regex re("\\bint\\s+(" + cv + ")\\b(\\s*\\[\\d+\\])?");
                sl = std::regex_replace(sl, re, "color $1$2");
            }
            f << "    " << sl << "\n";
        }
        f << "    noLoop();\n";  // static mode: run setup() once, stop
        f << "}\n";
        f << "void draw() {}\n";
    } else {
        // Structured sketch -- paste as-is (already has setup/draw defined)
        // Pre-pass 1: hoist class definitions to top (they may be used before defined).
        // Pre-pass 2: collect top-level bare function calls into setup().
        {
            // Separate code into: class blocks, and everything else
            std::vector<std::string> classLines;   // complete class definitions
            std::vector<std::string> toInject;     // bare calls to inject into setup()
            std::vector<std::string> otherLines;   // everything else

            int depth = 0;
            bool inClass = false;
            std::vector<std::string> classBuf;

            for (auto& rawl : code) {
                std::string tr = rawl;
                size_t p0 = tr.find_first_not_of(" \t");
                if (p0 != std::string::npos) tr = tr.substr(p0);

                // Detect class start at depth 0
                if (depth == 0 && !inClass) {
                    bool startsClass = (tr.rfind("class ", 0) == 0 || tr.rfind("struct ", 0) == 0);
                    if (startsClass) {
                        inClass = true;
                        classBuf.clear();
                    }
                }

                if (inClass) {
                    classBuf.push_back(rawl);
                    for (char c : rawl) { if(c=='{') depth++; else if(c=='}') depth--; }
                    // Only end class when we've seen at least one { AND depth returns to 0
                    if (depth == 0 && classBuf.size() > 1) {
                        for (auto& cl : classBuf) classLines.push_back(cl);
                        classBuf.clear();
                        inClass = false;
                    }
                    continue;
                }

                for (char c : rawl) { if(c=='{') depth++; else if(c=='}') depth--; }

                // At depth 0: check for bare function calls
                if (depth == 0) {
                    bool looksLikeCall = !tr.empty()
                        && std::isalpha((unsigned char)tr[0])
                        && tr.find('(') != std::string::npos
                        && tr.find(';') != std::string::npos;
                    bool hasTypeBefore = false;
                    if (looksLikeCall) {
                        size_t paren = tr.find('(');
                        std::string before = tr.substr(0, paren);
                        hasTypeBefore = before.find(' ') != std::string::npos;
                    }
                    bool isComment = tr.size()>1 && tr[0]=='/' && (tr[1]=='/'||tr[1]=='*');
                    if (looksLikeCall && !hasTypeBefore && !isComment) {
                        toInject.push_back(rawl);
                        continue;
                    }
                }
                otherLines.push_back(rawl);
            }

            // Output: classes first, then everything else
            auto emitLine = [&](const std::string& l) {
                std::string sl = sanitizeLine(l);
                for (auto& cv : colorVars) {
                    std::regex re("\\bint\\s+(" + cv + ")\\b(\\s*\\[\\d+\\])?");
                    sl = std::regex_replace(sl, re, "color $1$2");
                }
                f << sl << "\n";
            };

            for (auto& l : classLines) emitLine(l);

            bool injected = false;
            for (auto& l : otherLines) {
                emitLine(l);
                if (!injected && !toInject.empty()) {
                    std::string tr2 = sanitizeLine(l);
                    size_t p2 = tr2.find_first_not_of(" \t");
                    if (p2 != std::string::npos) tr2 = tr2.substr(p2);
                    if (tr2 == "void setup() {") {
                        for (auto& inj : toInject)
                            f << "    " << sanitizeLine(inj) << "\n";
                        injected = true;
                    }
                }
            }
        }
        // (loop handled above)
        if(false) for (auto& l : code) {            std::string sl = sanitizeLine(l);
            for (auto& cv : colorVars) {
                // Replace `int varname` and `int varname[N]` with color equivalents
                std::regex re("\\bint\\s+(" + cv + ")\\b(\\s*\\[\\d+\\])?");
                sl = std::regex_replace(sl, re, "color $1$2");
            }
            f << sl << "\n";
        }
    }

    // Generate wireCallbacks() -- assigns only the callbacks the sketch defines.
    // This is the cross-platform solution: no weak symbols, no undefined refs.
    // Note: we are ALREADY inside namespace Processing { ... } -- no extra wrapper.
    f << "\nstatic void _sketchWire() {\n";
    // Scan code to see which callbacks the sketch defines
    auto hasFn = [&](const std::string& sig) {
        for (auto& l : code)
            if (l.find(sig) != std::string::npos) return true;
        return false;
    };
    if (hasFn("void keyPressed("))   f << "    _onKeyPressed   = keyPressed;\n";
    if (hasFn("void keyReleased("))  f << "    _onKeyReleased  = keyReleased;\n";
    if (hasFn("void keyTyped("))     f << "    _onKeyTyped     = keyTyped;\n";
    if (hasFn("void mousePressed(")) f << "    _onMousePressed = mousePressed;\n";
    if (hasFn("void mouseReleased("))f << "    _onMouseReleased= mouseReleased;\n";
    if (hasFn("void mouseClicked(")) f << "    _onMouseClicked = mouseClicked;\n";
    if (hasFn("void mouseMoved("))   f << "    _onMouseMoved   = mouseMoved;\n";
    if (hasFn("void mouseDragged(")) f << "    _onMouseDragged = mouseDragged;\n";
    if (hasFn("void mouseWheel("))   f << "    _onMouseWheel   = mouseWheel;\n";
    if (hasFn("void windowMoved("))  f << "    _onWindowMoved  = windowMoved;\n";
    if (hasFn("void windowResized("))f << "    _onWindowResized= windowResized;\n";
    f << "}\n";
    // Wire up via a global initializer (runs before main)
    f << "static int _autoWire = []{ _wireCallbacksFn = _sketchWire; return 0; }();\n";
    if (isTopLevel) {
        // For static sketches: register setup() as the redraw function
        // so tiling WMs (i3) can trigger a redraw when the window is resized.
        f << "static int _autoRedraw = []{ _staticSketchSetup = setup; return 0; }();\n";
    }

    f << "} // namespace Processing\n";
    return true;
}

// =============================================================================
// SKETCH PROCESS (background output capture)
// =============================================================================

static void stopSketch() {
    sketchRunning = false;
    plat_proc_stop(sketchProc);
    if (sketchThread.joinable()) sketchThread.detach();
}

// Path of the currently-running binary -- deleted after the sketch exits
static std::string runningBinPath;

static void sketchReaderThread(int /*unused*/) {
    char buf[4096];
    std::string partial;

    auto pushLine = [&](std::string line) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // Hard-wrap long lines
        while ((int)line.size() > WRAP_COLS) {
            { std::lock_guard<std::mutex> lk(outMutex);
              outLines.push_back(line.substr(0, WRAP_COLS));
              outScroll = std::max(0, (int)outLines.size() - 14); }
            line = line.substr(WRAP_COLS);
        }
        if (!line.empty()) {
            std::lock_guard<std::mutex> lk(outMutex);
            outLines.push_back(line);
            outScroll = std::max(0, (int)outLines.size() - 14);
        }
    };

    while (sketchRunning.load()) {
        int n = plat_proc_read(sketchProc, buf, (int)sizeof(buf)-1);
        if (n > 0) {
            buf[n] = 0;
            partial += buf;
            size_t pos;
            while ((pos = partial.find('\n')) != std::string::npos) {
                pushLine(partial.substr(0, pos));
                partial = partial.substr(pos + 1);
            }
            // Flush partial line if it's getting long (handles print() spam)
            while ((int)partial.size() >= WRAP_COLS) {
                pushLine(partial.substr(0, WRAP_COLS));
                partial = partial.substr(WRAP_COLS);
            }
        } else if (n == 0) {
            break; // EOF
        }
        if (!plat_proc_running(sketchProc)) {
            // Drain remaining bytes
            while ((n = plat_proc_read(sketchProc, buf, sizeof(buf)-1)) > 0) {
                buf[n] = 0; partial += buf;
                size_t pos;
                while ((pos = partial.find('\n')) != std::string::npos) {
                    pushLine(partial.substr(0, pos));
                    partial = partial.substr(pos + 1);
                }
            }
            break;
        }
        plat_sleep_ms(8);
    }
    if (!partial.empty()) pushLine(partial);
    { std::lock_guard<std::mutex> lk(outMutex);
      outLines.push_back("-- sketch exited --");
      outScroll = std::max(0, (int)outLines.size() - 14); }
    sketchRunning = false;
    // Debugger mode: signal main thread to advance to next example
    if (debuggerMode) {
        debuggerIndex++;
        debuggerAdvance = true; // main thread will pick this up
    }
    // Delete the binary after the sketch exits (like Processing cleans up temp files)
    if (!runningBinPath.empty()) {
        plat_sleep_ms(50);           // brief pause so OS releases the file handle
        std::remove(runningBinPath.c_str());
        runningBinPath.clear();
    }
}

// =============================================================================
// BUILD & RUN
// =============================================================================

// Build thread worker -- runs g++ and streams output into outLines.
// Runs on a background thread so the IDE stays responsive.
static void buildWorker(const std::string& cmd, const std::string& outBin) {
    buildProgress.store(0.05f); // show some immediate progress

#ifdef _WIN32
    // Use plat_popen so no black console window flashes during compilation
    auto pp = plat_popen(cmd);
    FILE* pipe = pp.f;
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) {
        std::lock_guard<std::mutex> lk(outMutex);
        outLines.push_back("ERROR: could not launch compiler -- is g++ in PATH?");
        hasError = true;
        isBuilding.store(false);
        buildDone.store(true);
        buildSucceeded.store(false);
        buildProgress.store(0.f);
        return;
    }

    buildProgress.store(0.15f);

    char buf[512];
    while (fgets(buf, sizeof(buf), pipe)) {
        std::string s(buf);
        while (!s.empty() && (s.back()=='\n'||s.back()=='\r')) s.pop_back();
        if (!s.empty()) {
            std::lock_guard<std::mutex> lk(outMutex);
            if (s.find("error:") != std::string::npos) {
                hasError = true;
                buildProgress.store(1.f);   // jump to end on error
            }
            outLines.push_back(s);
            outScroll = std::max(0, (int)outLines.size() - 10);
        }
        // Fake incremental progress while compiling
        float p = buildProgress.load();
        if (p < 0.90f) buildProgress.store(p + 0.001f);
    }

#ifdef _WIN32
    int buildRet = plat_pclose(pp);
#else
    int buildRet = pclose(pipe);
#endif
    (void)buildRet;

    buildProgress.store(0.95f);

    {
        std::lock_guard<std::mutex> lk(outMutex);
        if (!hasError) {
            outLines.push_back("Built: " + outBin + "   (Ctrl+R to run)");
            buildSucceeded.store(true);
        } else {
            int errCount = 0, warnCount = 0;
            for (auto& ol : outLines) {
                if (ol.find("error:")   != std::string::npos) errCount++;
                if (ol.find("warning:") != std::string::npos) warnCount++;
            }
            outLines.push_back("Build failed: " + std::to_string(errCount) + " error(s)" +
                (warnCount > 0 ? ", " + std::to_string(warnCount) + " warning(s)" : ""));
            buildSucceeded.store(false);
        }
        outScroll = std::max(0, (int)outLines.size() - 10);
    }

    buildProgress.store(1.f);
    isBuilding.store(false);
    buildDone.store(true);
}

// Called from the main thread (button click / Ctrl+B).
// Starts the build on a background thread and returns immediately.
static void doCompile(bool thenRun=false) {
    if (isBuilding.load()) return;  // already building

    outLines.clear();
    hasError   = false;
    outScroll  = 0;
    buildDone.store(false);
    buildSucceeded.store(false);
    buildProgress.store(0.f);

    if (!writeSketch()) return;

    // Derive output binary name from current filename
    sketchBin = "SketchApp";
    if (!currentFile.empty()) {
        std::string base = currentFile;
        size_t sl = base.rfind('/');
        if (sl == std::string::npos) sl = base.rfind('\\');
        if (sl != std::string::npos) base = base.substr(sl + 1);
        if (base.size() > 4 && base.substr(base.size()-4) == ".cpp")
            base = base.substr(0, base.size()-4);
        sketchBin = base;
    }

#ifdef _WIN32
    std::string ext = ".exe";
#else
    std::string ext = "";
#endif
    std::string outBin = sketchBin + ext;

    // Speed optimization: compile Processing.cpp to Processing.o once,
    // then reuse it. Only Sketch_run.cpp changes each run.
    // On Windows this cuts compile time from ~8s to ~2s.
    static bool processingObjBuilt = false;
    // Precompiled header for Processing.h -- massively speeds up sketch compilation.
    // Rebuild PCH only when Processing.h is newer than the .gch file.
    static bool pchBuilt = false;
    bool needPch = !pchBuilt || !plat_file_exists("src/Processing.h.gch");
    if (!needPch && plat_file_exists("src/Processing.h") && plat_file_exists("src/Processing.h.gch")) {
        struct stat hst, gst;
        if (stat("src/Processing.h",&hst)==0 && stat("src/Processing.h.gch",&gst)==0)
            if (hst.st_mtime > gst.st_mtime) { needPch = true; pchBuilt = false; }
    }
    if (needPch) {
        outLines.push_back("[build] Precompiling Processing.h...");
        std::string pchCmd = "g++ -std=c++17 -x c++-header src/Processing.h -o src/Processing.h.gch " + buildFlags;
#ifndef _WIN32
        pchCmd += " 2>&1";
#endif
#ifdef _WIN32
        auto _pch = plat_popen(pchCmd);
        if (_pch.f) { char buf[256]; while(fgets(buf,sizeof(buf),_pch.f)){} plat_pclose(_pch); }
#else
        FILE* _pf = popen(pchCmd.c_str(),"r");
        if (_pf) { char buf[256]; while(fgets(buf,sizeof(buf),_pf)){} pclose(_pf); }
#endif
        pchBuilt = plat_file_exists("src/Processing.h.gch");
    }
    // Use PCH for sketch compilation via -include Processing.h
    // (GCC automatically uses the .gch if it's in the same directory)

    // Force rebuild if Processing.cpp is newer than Processing.o
    if (plat_file_exists("src/Processing.o") && plat_file_exists("src/Processing.cpp")) {
        struct stat cpp_st, obj_st;
        if (stat("src/Processing.cpp", &cpp_st)==0 && stat("src/Processing.o", &obj_st)==0) {
            if (cpp_st.st_mtime > obj_st.st_mtime) {
                processingObjBuilt = false; // force rebuild
                outLines.push_back("[build] Processing.cpp changed -- rebuilding Processing.o");
            }
        }
    }

    if (!processingObjBuilt || !plat_file_exists("src/Processing.o")) {
        std::string preCmd =
            "g++ -std=c++17 -c"
            " src/Processing.cpp"
            " -o src/Processing.o"
            " " + buildFlags +
#ifndef _WIN32
            " 2>&1" +
#endif
            "";
#ifdef _WIN32
        auto _pp0 = plat_popen(preCmd);
        if (_pp0.f) { char buf[256]; while(fgets(buf,sizeof(buf),_pp0.f)){} plat_pclose(_pp0); }
#else
        FILE* _pp0 = popen(preCmd.c_str(),"r");
        if (_pp0) { char buf[256]; while(fgets(buf,sizeof(buf),_pp0)){} pclose(_pp0); }
#endif
        processingObjBuilt = plat_file_exists("src/Processing.o");
    }

    // Build the compile command -- link prebuilt Processing.o with sketch
    std::string processingObj = processingObjBuilt ? "src/Processing.o" : "src/Processing.cpp";
    // -include src/Processing.h makes GCC use src/Processing.h.gch automatically
    std::string pchFlag = plat_file_exists("src/Processing.h.gch") ?
        " -include src/Processing.h" : "";
    std::string cmd =
        "g++ -std=c++17"
        " " + processingObj +
        " src/Sketch_run.cpp"
        " src/Processing_defaults.cpp"
        " src/main.cpp"
        " -o \"" + outBin + "\"" +
        " " + buildFlags + pchFlag +
#ifndef _WIN32
        " 2>&1" +  // on Linux/Mac: merge stderr into stdout for popen()
#endif
        "";

    outLines.push_back("Building: " + sketchBin);
    outLines.push_back("$ " + cmd);

    isBuilding.store(true);

    // Launch build on background thread, optionally run after
    buildThread = std::thread([cmd, outBin, thenRun]() {
        buildWorker(cmd, outBin);
        // If caller wanted run-after-build, trigger it on success
        if (thenRun && buildSucceeded.load()) {
            // Signal main thread to launch sketch
            // (can't call doRun() from here -- it touches OpenGL)
            // We set a flag that draw() checks
        }
    });
    buildThread.detach();
}

// Set to true by Ctrl+R / Run button.  draw() polls this and launches
// the sketch once the build thread reports success.
static bool pendingRun = false;

static void launchSketch() {
#ifdef _WIN32
    std::string bin = sketchBin + ".exe";
#else
    std::string bin = sketchBin;       // built without ./ prefix; stored as-is
    if (bin.find('/') == std::string::npos) bin = "./" + bin;
#endif
    if (!plat_file_exists(bin)) {
        outLines.push_back("ERROR: binary not found: " + bin);
        outScroll = std::max(0, (int)outLines.size() - 10);
        return;
    }
    stopSketch();
    runningBinPath = bin;              // remember for post-exit deletion
    sketchProc = plat_proc_start(bin);
    if (!plat_proc_ok(sketchProc)) {
        outLines.push_back("ERROR: failed to launch " + bin);
        outScroll = std::max(0, (int)outLines.size() - 10);
        runningBinPath.clear();
        return;
    }
    sketchRunning = true;
    {
        std::lock_guard<std::mutex> lk(outMutex);
        outLines.push_back("Running: " + bin);
        outLines.push_back("------------------------------------------------------");
    }
    sketchThread = std::thread(sketchReaderThread, 0);
    sketchThread.detach();
    outScroll = std::max(0, (int)outLines.size() - 10);
}

static void doRun() {
    // Start async build; pendingRun=true tells draw() to launch on success
    pendingRun = true;
    doCompile();
    if (isBuilding.load()) return; // build started async, draw() will launch

    // If build was instant (already cached) handle immediately
    if (hasError) {
        pendingRun = false;
        outLines.push_back("Not running -- fix errors first.");
        outScroll = std::max(0, (int)outLines.size() - 10);
        return;
    }

    launchSketch();
}

static void doStop() {
    if (sketchRunning) {
        stopSketch();
        outLines.push_back("-- Sketch stopped --");
        outScroll = std::max(0, (int)outLines.size() - 10);
        // Delete binary on manual stop (Processing discards its temp build on stop too)
        if (!runningBinPath.empty()) {
            plat_sleep_ms(80);       // give OS time to release the process handle
            std::remove(runningBinPath.c_str());
            runningBinPath.clear();
        }
    }
}

// =============================================================================
// SYNTAX HIGHLIGHTING
// =============================================================================

static const std::vector<std::string> KEYWORDS = {
    // C++ keywords
    "void","int","float","double","bool","char","auto","long","short",
    "unsigned","signed","true","false","null","nullptr","return",
    "if","else","for","while","do","switch","case","break","continue",
    "default","new","delete","class","struct","template","typename",
    "namespace","static","const","constexpr","override","virtual",
    "public","private","protected","inline","extern","typedef","using",
    // Processing API
    "size","background","fill","stroke","noStroke","noFill",
    "ellipse","rect","circle","line","triangle","quad","point","arc",
    "bezier","curve","box","sphere","translate","rotate","scale",
    "rotateX","rotateY","rotateZ","shearX","shearY",
    "pushMatrix","popMatrix","push","pop",
    "beginShape","endShape","vertex","bezierVertex","curveVertex",
    "width","height","mouseX","mouseY","pmouseX","pmouseY",
    "_mousePressed","_keyPressed","mouseButton","keyCode",
    "frameCount","frameRate","millis","second","minute","hour",
    "day","month","year","PI","TWO_PI","HALF_PI","QUARTER_PI",
    "map","constrain","lerp","norm","sqrt","sq","abs","pow",
    "floor","ceil","round","sin","cos","tan","asin","acos",
    "atan","atan2","degrees","radians","dist","mag",
    "noise","random","randomSeed","noiseSeed",
    "color","PVector","setup","draw",
    "text","textSize","textFont","textAlign","textWidth",
    "loadImage","image","tint","noTint","blendMode","colorMode",
    "lerpColor","red","green","blue","alpha","hue","saturation","brightness",
    "println","print","save","saveFrame","exit","loop","noLoop","redraw",
    "lights","noLights","ambientLight","directionalLight",
};

struct Tok { std::string s; int r, g, b; };

static std::vector<Tok> tokenize(const std::string& ln) {
    std::vector<Tok> out;
    int n = (int)ln.size(), i = 0;
    while (i < n) {
        // Line comment
        if (i+1 < n && ln[i] == '/' && ln[i+1] == '/') {
            out.push_back({ ln.substr(i), 106, 153, 85 });
            break;
        }
        // Preprocessor
        if (ln[i] == '#') {
            out.push_back({ ln.substr(i), 197, 134, 192 });
            break;
        }
        // String literal
        if (ln[i] == '"') {
            int j = i+1;
            while (j < n && ln[j] != '"') j++;
            out.push_back({ ln.substr(i, j-i+1), 206, 145, 120 });
            i = j+1; continue;
        }
        // Angle-bracket include: only color <...> as string after #include
        // Regular < (comparison, template) is treated as a plain operator
        if (ln[i] == '<') {
            // Check if this line starts with #include
            std::string trimmed = ln.substr(0, i);
            bool isInclude = (trimmed.find("#include") != std::string::npos);
            if (isInclude) {
                int j = i+1;
                while (j < n && ln[j] != '>') j++;
                out.push_back({ ln.substr(i, j-i+1), 206, 145, 120 });
                i = j+1; continue;
            }
            // Otherwise treat < as a plain operator character
            out.push_back({ "<", 200, 200, 200 });
            i++; continue;
        }
        // Char literal
        if (ln[i] == '\'' && i+2 < n) {
            out.push_back({ ln.substr(i, 3), 206, 145, 120 });
            i += 3; continue;
        }
        // Number
        if (isdigit((unsigned char)ln[i])) {
            int j = i;
            while (j < n && (isdigit((unsigned char)ln[j]) || ln[j]=='.'||ln[j]=='f'||ln[j]=='x')) j++;
            out.push_back({ ln.substr(i, j-i), 181, 206, 168 });
            i = j; continue;
        }
        // Identifier / keyword / function call
        if (isalpha((unsigned char)ln[i]) || ln[i] == '_') {
            int j = i;
            while (j < n && (isalnum((unsigned char)ln[j]) || ln[j]=='_')) j++;
            std::string w = ln.substr(i, j-i);
            bool kw = std::find(KEYWORDS.begin(), KEYWORDS.end(), w) != KEYWORDS.end();
            bool fn = (j < n && ln[j] == '(');
            if      (kw) out.push_back({ w, 112, 184, 255 });
            else if (fn) out.push_back({ w, 220, 220, 170 });
            else         out.push_back({ w, 156, 220, 254 });
            i = j; continue;
        }
        out.push_back({ std::string(1, ln[i]), 200, 200, 200 });
        i++;
    }
    return out;
}

// =============================================================================
// DRAW HELPERS
// =============================================================================

static void qFill(float x, float y, float w, float h, int r, int g, int b, int a=255) {
    noStroke(); fill(r, g, b, a); rect(x, y, w, h);
}
static void qBorder(float x, float y, float w, float h, int r, int g, int b) {
    noFill(); stroke(r, g, b); strokeWeight(1); rect(x, y, w, h); noStroke();
}
static void qLine(float x1, float y1, float x2, float y2, int r, int g, int b) {
    stroke(r, g, b); strokeWeight(1); line(x1, y1, x2, y2); noStroke();
}
static void iText(const std::string& s, float x, float y, int r, int g, int b, float sz) {
    textSize(sz); fill(r, g, b); noStroke(); text(s, x, y);
}
static float xOf(int ln, int col) {
    textSize(FS);
    if (col == 0) return (float)(sbW() + GUTTER_W + 4);
    return sbW() + GUTTER_W + 4 + textWidth(code[ln].substr(0, col));
}
static void setClip(const std::string& s) { setClipboard(s); }
static std::string getClip() { return getClipboard(); }

// =============================================================================
// SIDEBAR DRAW
// =============================================================================

static void drawSidebar() {
    if (!sidebarVisible) return;
    int sw = SIDEBAR_W;

    qFill(0, editorY(), sw, height - editorY(), 26, 26, 26);
    qLine(sw, editorY(), sw, height, 50, 50, 50);

    // Resize handle (right edge)
    bool onEdge = mouseX >= sw-4 && mouseX <= sw+2 && mouseY >= editorY();
    qFill(sw-4, editorY(), 4, height - editorY(),
          (onEdge||sidebarResizing) ? 17 : 26,
          (onEdge||sidebarResizing) ? 108 : 27,
          (onEdge||sidebarResizing) ? 179 : 36);

    // Header
    qFill(0, editorY(), sw, 24, 32, 32, 32);
    iText("EXPLORER", 10, editorY()+17, 130, 135, 165, FST);

    // Open folder button
    float bx = sw-60, by = editorY()+4, bw = 54, bh = 16;
    bool bHov = mouseX>=bx && mouseX<=bx+bw && mouseY>=by && mouseY<=by+bh;
    qFill(bx, by, bw, bh, bHov?34:26, bHov?108:30, bHov?179:46);
    qBorder(bx, by, bw, bh, bHov?60:45, bHov?160:55, bHov?240:80);
    iText("Open..", bx+4, by+bh*0.82f, bHov?230:160, bHov?240:170, bHov?255:210, 10.0f);

    // Root label
    std::string rootLabel = ftRoot;
    if (rootLabel.size() > 22) rootLabel = ".." + rootLabel.substr(rootLabel.size()-20);
    iText(rootLabel, 8, editorY()+42, 150, 175, 210, FST);

    // File tree
    int treeTop = editorY() + 52;
    float rowH  = FSS * 1.7f;
    int   vis   = (int)((height - treeTop) / rowH);

    // Show hint when no folder opened yet
    if (ftEntries.empty()) {
        iText("Click Open to",  10, treeTop + rowH,      100, 130, 170, FST);
        iText("browse files.",  10, treeTop + rowH*2.2f, 100, 130, 170, FST);
    }

    ftScroll = std::max(0, std::min(ftScroll, std::max(0, (int)ftEntries.size() - vis)));
    for (int i = 0; i < vis; i++) {
        int fi = ftScroll + i;
        if (fi >= (int)ftEntries.size()) break;
        auto& fe = ftEntries[fi];
        float ry = treeTop + i * rowH;
        float rx = 8 + fe.depth * 12;
        bool hov = mouseX >= 0 && mouseX < sw-6 && mouseY >= ry && mouseY < ry+rowH;
        if (hov) qFill(0, ry, sw-6, rowH, 38, 38, 38);
        std::string icon = fe.isDir ? (fe.expanded ? "v " : "> ") : "  ";
        int ir = fe.isDir ? 200 : 160;
        int ig = fe.isDir ? 180 : 185;
        int ib = fe.isDir ? 100 : 210;
        iText(icon + fe.name, rx, ry + rowH*0.75f, ir, ig, ib, FST);
    }
}

// =============================================================================
// MENU BAR DRAW
// =============================================================================

struct MenuItem { std::string label, shortcut; };

static void drawDropdown(float mx, float my, const std::vector<MenuItem>& items) {
    float pw = 210, ph = items.size() * 22 + 8;
    qFill(mx, my, pw, ph, 45, 45, 45);
    qBorder(mx, my, pw, ph, 70, 70, 70);
    for (int i = 0; i < (int)items.size(); i++) {
        float ry = my + 4 + i * 22;
        if (items[i].label == "---") { qFill(mx+4, ry+10, pw-8, 1, 65, 65, 65); continue; }
        bool hov = mouseX>=mx && mouseX<=mx+pw && mouseY>=ry && mouseY<=ry+22;
        if (hov) qFill(mx+1, ry, pw-2, 22, 17, 108, 179);
        iText(items[i].label, mx+10, ry+16, hov?255:215, hov?255:218, hov?255:228, FST);
        if (!items[i].shortcut.empty()) {
            textSize(FST);
            float sw = textWidth(items[i].shortcut);
            iText(items[i].shortcut, mx+pw-sw-8, ry+16, hov?220:120, hov?220:123, hov?220:145, FST);
        }
    }
}

static void drawMenuBar() {
    qFill(0, 0, width, MENUBAR_H, 36, 36, 36);
    qLine(0, MENUBAR_H-1, width, MENUBAR_H-1, 58, 58, 58);

    struct Head { std::string label; Menu id; float x=0, w=0; };
    std::vector<Head> heads = {
        {"File",Menu::File}, {"Edit",Menu::Edit},
        {"Sketch",Menu::Sketch}, {"Tools",Menu::Tools},
        {"Libraries",Menu::Libraries}
    };
    textSize(FST);
    float mx = 6;
    for (auto& h : heads) { h.x = mx; h.w = textWidth(h.label)+14; mx += h.w+2; }
    for (auto& h : heads) {
        bool open = (openMenu == h.id);
        bool hov  = mouseX>=h.x-2 && mouseX<=h.x+h.w && mouseY>=0 && mouseY<MENUBAR_H;
        if (open || hov) qFill(h.x-2, 0, h.w, MENUBAR_H, 17, 108, 179);
        iText(h.label, h.x+4, MENUBAR_H*0.74f, 228, 230, 242, FST);
    }

    if (openMenu==Menu::File)
        drawDropdown(2, MENUBAR_H, {{"New","Ctrl+N"},{"Open...","Ctrl+O"},{"---",""},{"Save","Ctrl+S"},{"Save As...","Ctrl+Shift+S"},{"---",""},{"Show Sketch Folder",""},{"Export Application...",""},{"---",""},{"Exit",""}});
    else if (openMenu==Menu::Edit)
        drawDropdown(42, MENUBAR_H, {{"Undo","Ctrl+Z"},{"Redo","Ctrl+Y"},{"---",""},{"Cut","Ctrl+X"},{"Copy","Ctrl+C"},{"Paste","Ctrl+V"},{"---",""},{"Select All","Ctrl+A"},{"Duplicate Line","Ctrl+D"},{"---",""},{"Toggle Comment","Ctrl+/"},{"Auto Format","Ctrl+Shift+F"}});
    else if (openMenu==Menu::Sketch)
        drawDropdown(84, MENUBAR_H, {{"Build","Ctrl+B"},{"Run","Ctrl+R"},{"Stop","Ctrl+."}});
    else if (openMenu==Menu::Tools)
        drawDropdown(138, MENUBAR_H, {{"Examples","Ctrl+Shift+E"},{"---",""},{"Auto Format","Ctrl+Shift+F"},{"---",""},{"Increase Font","Ctrl+="},{"Decrease Font","Ctrl+-"}});
    else if (openMenu==Menu::Libraries)
        drawDropdown(182, MENUBAR_H, {{"Manage Libraries...","Ctrl+Shift+L"},{"---",""},{"Add #include",""}});
}

// =============================================================================
// TOOLBAR DRAW
// =============================================================================

static void drawToolbar() {
    int ty = MENUBAR_H;
    qFill(0, ty, width, TOOLBAR_H, 42, 42, 42);
    qLine(0, ty+TOOLBAR_H-1, width, ty+TOOLBAR_H-1, 58, 58, 58);

    // File title
    std::string title = currentFile.empty() ? "untitled" : currentFile;
    if (modified) title += " *";
    iText(title, 10, ty+TOOLBAR_H*0.68f, 170, 180, 220, FSS);

    // Vim badge

    // Examples button
    float epx = width-300, epy = ty+6, epw = 80, eph = TOOLBAR_H-12;
    bool  epH = mouseX>=epx && mouseX<=epx+epw && mouseY>=epy && mouseY<=epy+eph;
    qFill(epx, epy, epw, eph, epH?38:26, epH?120:85, epH?70:52);
    qBorder(epx, epy, epw, eph, 55, 160, 100);
    iText("Examples", epx+5, epy+eph*0.72f, epH?210:170, epH?245:215, epH?220:190, FST);

    // Build button (greyed out while building)
    float bx = width-196, by = ty+6, bh = TOOLBAR_H-12, bw = 92;
    bool  bH = !isBuilding.load() && mouseX>=bx && mouseX<=bx+bw && mouseY>=by && mouseY<=by+bh;
    bool  building = isBuilding.load();
    qFill(bx, by, bw, bh, building?50:(bH?217:160), building?50:(bH?119:80), building?55:(bH?87:55));
    qBorder(bx, by, bw, bh, building?70:(bH?217:180), building?70:(bH?119:90), building?75:(bH?87:55));
    iText(building?"Building...":"Build", bx+6, by+bh*0.72f,
          building?140:255, building?150:235, building?160:225, FSS);

    // Run button (greyed out while building)
    float rx = width-96;
    bool  rH = !isBuilding.load() && mouseX>=rx && mouseX<=rx+bw && mouseY>=by && mouseY<=by+bh;
    qFill(rx, by, bw, bh, building?30:(rH?25:17), building?30:(rH?130:108), building?35:(rH?210:179));
    qBorder(rx, by, bw, bh, building?45:( rH?70:40), building?45:(rH?160:120), building?50:(rH?220:180));
    iText("Run", rx+14, by+bh*0.72f, building?120:230, building?130:240, building?140:255, FSS);

    // Status dot: orange=running, red=error, green=built OK, blue=building
    if      (building)       qFill(width-210, by+bh/2-5, 10, 10, 17, 108, 255);
    else if (sketchRunning)  qFill(width-210, by+bh/2-5, 10, 10, 255, 160, 0);
    else if (hasError)       qFill(width-210, by+bh/2-5, 10, 10, 220, 60, 60);
    else if (!outLines.empty() && outLines.back().find("Built:") != std::string::npos)
                             qFill(width-210, by+bh/2-5, 10, 10, 60, 200, 60);
}

// =============================================================================
// STATUS BAR DRAW
// =============================================================================

static void drawStatus() {
    int sy = statusY();
    qFill(0, sy, width, STATUS_H, 30, 30, 30);
    qLine(0, sy, width, sy, 55, 55, 55);

    std::string s = "Ln " + std::to_string(curLine+1) +
                    "  Col " + std::to_string(curCol+1) +
                    "  |  " + std::to_string((int)code.size()) + " lines";
    if (!currentFile.empty()) s += "  |  " + currentFile;
    if (!sketchBin.empty())   s += "  >  ./" + sketchBin;
    iText(s, 8, sy+STATUS_H*0.76f, 125, 130, 158, FST);

    std::string right = "UTF-8  C++17";
    textSize(FST);
    iText(right, width - textWidth(right) - 8, sy+STATUS_H*0.76f,
          90, 95, 110, FST);
}

// =============================================================================
// EDITOR DRAW
// =============================================================================

static void drawEditor() {
    int   ey  = editorY(), eh = editorH();
    float lh  = lineH(), asc = FS * 0.80f;
    int   ex  = sbW(), ew = editorFullW();

    qFill(ex, ey, ew, eh, 30, 30, 30);          // editor bg
    qFill(ex, ey, GUTTER_W, eh, 38, 38, 38);    // gutter bg
    qLine(ex+GUTTER_W, ey, ex+GUTTER_W, ey+eh, 58, 58, 58);

    int vis = visLines();
    for (int i = 0; i < vis; i++) {
        int   li      = scrollTop + i;
        if (li >= (int)code.size()) break;
        float rowTop  = ey + i * lh;
        float baseline= rowTop + asc + 2;

        // Current line highlight
        if (li == curLine)
            qFill(ex+GUTTER_W, rowTop, ew-GUTTER_W, lh, 44, 44, 50);

        // Selection highlight
        if (hasSel()) {
            int l0, c0, l1, c1;
            selRange(l0, c0, l1, c1);
            if (li >= l0 && li <= l1) {
                float sx  = (li == l0) ? xOf(li, c0) : (float)(ex+GUTTER_W+4);
                float ex2 = (li == l1) ? xOf(li, c1) : (float)(ex+ew-8);
                if (ex2 > sx) qFill(sx, rowTop+1, ex2-sx, lh-2, 17, 60, 110, 210);
            }
        }

        // Line number
        textSize(FST);
        std::string num = std::to_string(li+1);
        float nw = textWidth(num);
        if (li == curLine) iText(num, ex+GUTTER_W-6-nw, baseline-1, 200, 204, 215, FST);
        else               iText(num, ex+GUTTER_W-6-nw, baseline-1,  85,  90, 112, FST);

        // Syntax-highlighted tokens
        textSize(FS);
        float tx = ex + GUTTER_W + 4;
        for (auto& tok : tokenize(code[li])) {
            fill(tok.r, tok.g, tok.b); noStroke();
            text(tok.s, tx, baseline);
            tx += textWidth(tok.s);
        }

        // Cursor -- blinking line cursor
        if (li == curLine && (frameCount/22) % 2 == 0) {
            float cx = xOf(li, curCol);
            qFill(cx, rowTop+2, 2, lh-4, 220, 210, 160);
        }
    }

    // Minimap (right edge)
    static const int MM_W = 60;
    float mmx = (float)(ex + ew - MM_W - 8);
    qFill(mmx, ey, MM_W, eh, 24, 24, 24);
    qLine(mmx, ey, mmx, ey+eh, 48, 50, 62);
    if (!code.empty()) {
        float vpY = ey + (float)scrollTop / code.size() * eh;
        float vpH = std::max(8.0f, (float)vis / code.size() * eh);
        qFill(mmx+1, vpY, MM_W-2, vpH, 60, 80, 130, 120);
        for (int li = 0; li < (int)code.size(); li++) {
            float ly  = ey + (float)li / code.size() * eh;
            float lw2 = std::min((float)MM_W-4, (float)code[li].size() * 0.35f);
            if (lw2 > 0) qFill(mmx+2, ly, lw2, std::max(1.0f, (float)eh/code.size()), 55, 85, 120);
        }
    }

    // Vertical scrollbar (14px wide for easier interaction)
    static const int SB = 14;
    qFill(ex+ew-SB, ey, SB, eh, 40, 40, 40);
    if ((int)code.size() > vis) {
        float sbH = std::max(20.0f, (float)vis / code.size() * eh);
        float sbY = ey + (float)scrollTop / code.size() * eh;
        // Highlight on hover
        bool sbHov = (mouseX >= ex+ew-SB && mouseX <= ex+ew && mouseY >= ey && mouseY <= ey+eh);
        int  sbCol = sbHov ? 120 : 90;
        qFill(ex+ew-SB, sbY, SB, sbH, sbCol, sbCol, sbCol);
    }
}

// =============================================================================
// CONSOLE DRAW
// =============================================================================

// =============================================================================
// VIM TAB  --  vim operation cheat sheet
// =============================================================================

// The complete list of every supported vim operation with a description

static void drawConsole() {
    // Snapshot output lines to avoid holding lock during render
    std::vector<std::string> snap;
    { std::lock_guard<std::mutex> lk(outMutex); snap = outLines; }

    // Auto-scroll while sketch is running
    if (sketchRunning) {
        float lh2 = FSS * 1.5f;
        int   vis = std::max(1, (int)((CONSOLE_H - 4 - TAB_H) / lh2));
        outScroll = std::max(0, (int)snap.size() - vis);
    }


    int   cy  = consoleY(), cx = consoleX(), cw = consoleW();
    int   consH = (terminalPos == TermPos::Right) ? height - editorY() : CONSOLE_H;
    float lh  = FSS * 1.5f;

    // Resize handle
    bool onHandle = (terminalPos == TermPos::Bottom)
        ? (mouseY >= cy && mouseY <= cy+4)
        : (mouseX >= cx && mouseX <= cx+4 && mouseY >= cy && mouseY <= cy+consH);
    int hc = (onHandle||consoleResizing||termSideResizing) ? 108 : 50;
    if (terminalPos == TermPos::Bottom)
        qFill(cx, cy, cw, 4, 17*(onHandle?1:0), hc, 179*(onHandle?1:0)+75*(onHandle?0:1));
    else
        qFill(cx, cy, 4, consH, 17*(onHandle?1:0), hc, 179*(onHandle?1:0)+75*(onHandle?0:1));

    qFill(cx+4, cy+4, cw-4, consH-4, 20, 20, 20);

    // Tab bar
    qFill(cx+4, cy+4, cw-4, TAB_H, 24, 24, 24);
    qLine(cx+4, cy+4+TAB_H, cx+cw, cy+4+TAB_H, 52, 52, 52);
    float tx = (float)(cx + 8);
    for (int i = 0; i < (int)terminals.size(); i++) {
        textSize(FST);
        float tw = textWidth(terminals[i].name) + 20;
        bool  isA = (i == activeTab);
        bool  tH  = mouseX>=tx && mouseX<=tx+tw && mouseY>=cy+4 && mouseY<=cy+4+TAB_H;
        qFill(tx, cy+4, tw, TAB_H, isA?32:(tH?30:22), isA?33:(tH?31:23), isA?46:(tH?44:32));
        if (isA) qLine(tx, cy+4+TAB_H-1, tx+tw, cy+4+TAB_H-1, 17, 108, 179);
        if (terminals[i].hasError)           qFill(tx+tw-10, cy+4+7, 6, 6, 220, 60, 60);
        else if (!terminals[i].lines.empty())qFill(tx+tw-10, cy+4+7, 6, 6, 55, 190, 55);
        fill(isA?220:140, isA?224:145, isA?240:165);
        text(terminals[i].name, tx+6, cy+4+TAB_H-6);
        tx += tw + 2;
    }
    // (new tab button removed -- use terminals for multiple output views)



    // Running indicator
    if (sketchRunning)
        iText("* RUNNING", (float)(cx+cw-200), cy+4+TAB_H*0.75f, 80, 210, 100, FST);

    // Stop button
    if (sketchRunning) {
        float sx = (float)(cx+cw-168), sy2 = (float)(cy+3), sw2 = 60, sh = 16;
        bool sH = mouseX>=sx && mouseX<=sx+sw2 && mouseY>=sy2 && mouseY<=sy2+sh;
        qFill(sx, sy2, sw2, sh, sH?160:110, sH?40:30, sH?40:30);
        qBorder(sx, sy2, sw2, sh, 200, 60, 60);
        iText("Stop", sx+14, sy2+sh*0.82f, 255, 180, 180, 10.0f);
    }

    // Copy All button -- right side of tab bar
    {
        textSize(FST);
        float cbw  = 68;
        float cbx  = (float)(cx + cw - cbw - 8);
        float cby2 = (float)(cy + 5);
        float cbh  = (float)(TAB_H - 2);
        bool  cbH  = mouseX>=cbx && mouseX<=cbx+cbw && mouseY>=cby2 && mouseY<=cby2+cbh;
        qFill  (cbx, cby2, cbw, cbh, 22, 34, 62);
        qBorder(cbx, cby2, cbw, cbh, cbH?100:50, cbH?160:80, cbH?255:140);
        iText("Copy All", cbx+5, cby2+cbh*0.76f,
              cbH?255:200, cbH?255:210, cbH?255:240, FST);
    }

    // Output lines
    auto& tlines  = terminals[activeTab].lines;
    auto& tscroll = terminals[activeTab].scroll;
    int contentH  = consH - 4 - TAB_H;
    int visOut    = std::max(1, (int)(contentH / lh));
    tscroll = std::max(0, std::min(tscroll, std::max(0, (int)tlines.size() - visOut)));

    for (int i = 0; i < visOut; i++) {
        int li = tscroll + i;
        if (li >= (int)tlines.size()) break;
        auto& s = tlines[li];
        float rowTop  = cy + 4 + TAB_H + i * lh;
        float baseline= rowTop + lh - 3;

        bool hov = mouseX>cx+4 && mouseX<cx+cw-12 && mouseY>=rowTop && mouseY<rowTop+lh;
        bool sel = (li == consoleSelLine);
        if (sel)      qFill(cx+4, rowTop, cw-10, lh, 40, 60, 110);
        else if (hov) qFill(cx+4, rowTop, cw-10, lh, 30, 30, 30);

        // Colour-code by line type
        bool isErr  = s.find("error:")   != std::string::npos || s.find("X ") == 0;
        bool isWarn = s.find("warning:") != std::string::npos;
        bool isOk   = s.find("Built:")   != std::string::npos || s.find("OK ") == 0;
        bool isCmd  = !s.empty() && s[0] == '$';
        bool isSep  = !s.empty() && s[0] == '-';
        bool isBuild= isErr||isWarn||isOk||isCmd||isSep || s.find("Building:")!=std::string::npos;
        int r = 188, g = 192, b = 200;
        if      (isErr)  { r=255; g=90;  b=90;  }
        else if (isWarn) { r=255; g=190; b=50;  }
        else if (isOk)   { r=80;  g=215; b=100; }
        else if (isCmd)  { r=90;  g=170; b=255; }
        else if (isSep)  { r=55;  g=58;  b=72;  }
        else if (!isBuild){ r=228; g=220; b=160; } // sketch output -- warm yellow

        textSize(FSS);
        std::string disp = s;
        float maxW = (float)(cw - 30);
        if (textWidth(disp) > maxW) {
            int lo = 0, hi = (int)disp.size();
            while (lo < hi-1) {
                int mid = (lo+hi)/2;
                if (textWidth(disp.substr(0, mid)) < maxW) lo = mid; else hi = mid;
            }
            disp = disp.substr(0, lo);
            if (!disp.empty()) disp.back() = '>';
        }
        fill(r, g, b); noStroke(); text(disp, cx+14, baseline);
    }

    // Scrollbar
    if ((int)tlines.size() > visOut) {
        float th  = (float)contentH;
        float sbH = std::max(6.0f, (float)visOut / tlines.size() * th);
        float sbY = cy + 4 + TAB_H + (float)tscroll / tlines.size() * th;
        qFill(cx+cw-6, cy+4+TAB_H, 6, th, 38, 38, 38);
        qFill(cx+cw-6, sbY, 6, sbH, 80, 80, 80);
    }
}

// =============================================================================
// SERIAL MONITOR OVERLAY
// =============================================================================

static void drawExamples() {
    // Dim the background like other overlays
    qFill(0, 0, width, height, 0, 0, 0, 180);

    float pw = 580, ph = 480;
    float px = (width  - pw) * 0.5f;
    float py = (height - ph) * 0.5f;

    // Panel background
    qFill(px, py, pw, ph, 24, 25, 34);
    qBorder(px, py, pw, ph, 60, 65, 90);

    // Header bar
    qFill(px, py, pw, 36, 32, 34, 48);
    qLine(px, py+36, px+pw, py+36, 55, 58, 80);
    iText("Examples", px+14, py+25, 228, 232, 255, FS);

    // Refresh button
    float rBx = px+pw-90, rBy = py+8, rBw = 80, rBh = 22;
    bool  rBH = mouseX>=rBx&&mouseX<=rBx+rBw&&mouseY>=rBy&&mouseY<=rBy+rBh;
    qFill(rBx, rBy, rBw, rBh, rBH?35:24, rBH?80:55, rBH?55:38);
    qBorder(rBx, rBy, rBw, rBh, 55, 130, 90);
    iText("Refresh", rBx+10, rBy+rBh*0.78f, 140, 220, 160, FST);

    // Close button
    float xBx = px+pw-34, xBy = py+8, xBw = 24, xBh = 22;
    bool  xBH = mouseX>=xBx&&mouseX<=xBx+xBw&&mouseY>=xBy&&mouseY<=xBy+xBh;
    qFill(xBx, xBy, xBw, xBh, xBH?180:80, xBH?40:32, xBH?44:38);
    iText("X", xBx+7, xBy+xBh*0.78f, 240, 200, 200, FST);

    // Debug All button -- cycles through all examples automatically
    float dBx = px+pw-200, dBy = py+8, dBw = 100, dBh = 22;
    bool  dBH = mouseX>=dBx&&mouseX<=dBx+dBw&&mouseY>=dBy&&mouseY<=dBy+dBh;
    bool  dOn = debuggerMode;
    qFill(dBx, dBy, dBw, dBh, dOn?(dBH?60:50):( dBH?25:18), dOn?(dBH?140:120):(dBH?100:70), dOn?(dBH?255:220):(dBH?60:42));
    qBorder(dBx, dBy, dBw, dBh, dOn?80:40, dOn?180:110, dOn?255:160);
    iText(dOn?"■ Stop Debug":"▶ Debug All", dBx+6, dBy+dBh*0.78f, dOn?200:160, dOn?240:210, dOn?255:240, FST);

    // Column headers
    float hy = py + 44;
    qFill(px, hy, pw, 20, 32, 34, 48);
    iText("Sketch",      px+14,       hy+14, 120, 128, 165, FST);
    iText("Description", px+190,      hy+14, 120, 128, 165, FST);
    iText("Action",      px+pw-110,   hy+14, 120, 128, 165, FST);

    // List area
    float ly     = hy + 22;
    float rowH   = 38.f;
    int   visRows = (int)((ph - (ly - py) - 8) / rowH);

    if (exRows.empty()) {
        if (examplesList.empty()) {
            iText("No .cpp files found in examples/", px+20, ly+24, 140, 145, 170, FST);
            iText("Place .cpp files in examples/ or examples/foldername/", px+20, ly+44, 90, 95, 120, FST);
        }
    }

    exScroll = std::max(0, std::min(exScroll,
                std::max(0, (int)exRows.size() - visRows)));

    exHover = -1;
    for (int i = 0; i < visRows; i++) {
        int li = exScroll + i;
        if (li >= (int)exRows.size()) break;
        auto& row = exRows[li];
        float ry  = ly + i * rowH;
        bool  hov = mouseX>=px && mouseX<=px+pw && mouseY>=ry && mouseY<=ry+rowH;
        if (hov) exHover = li;

        if (row.isFolder) {
            // Folder header row
            bool open = exOpenFolders.count(row.folder) > 0;
            qFill(px, ry, pw, rowH, hov?38:28, hov?40:30, hov?62:44);
            qLine(px, ry+rowH-1, px+pw, ry+rowH-1, 55, 58, 80);
            // Arrow indicator
            iText(open ? "v" : ">", px+14, ry+rowH*0.72f, 160, 200, 255, FST);
            // Folder icon + name
            std::string lbl = "  " + row.label + "/";
            iText(lbl, px+28, ry+rowH*0.72f, 200, 215, 255, FS);
        } else {
            // File row -- indent if inside a folder
            float indent = row.folder.empty() ? 14.f : 32.f;
            qFill(px, ry, pw, rowH, hov?34:22, hov?36:24, hov?52:34);
            qLine(px, ry+rowH-1, px+pw, ry+rowH-1, 38, 40, 56);

            // Filename
            std::string nm = row.label;
            textSize(FST);
            while (nm.size()>2 && textWidth(nm)>175) nm.pop_back();
            iText(nm, px+indent, ry+rowH*0.70f, 210, 218, 248, FST);

            // Description
            std::string ds = row.desc;
            while (ds.size()>2 && textWidth(ds)>pw-290) ds.pop_back();
            iText(ds, px+190, ry+rowH*0.70f, 140, 148, 175, FST);

            // Open button
            float bx2 = px+pw-108, by2 = ry+6, bw2 = 50, bh2 = rowH-12;
            bool  bH  = mouseX>=bx2&&mouseX<=bx2+bw2&&mouseY>=by2&&mouseY<=by2+bh2;
            qFill(bx2, by2, bw2, bh2, bH?28:20, bH?100:72, bH?188:148);
            qBorder(bx2, by2, bw2, bh2, 40, 140, 210);
            iText("Open", bx2+8, by2+bh2*0.76f, 210, 230, 255, FST);

            // Run button
            float rx2 = bx2+54, ry2 = by2;
            bool  rH2 = mouseX>=rx2&&mouseX<=rx2+bw2&&mouseY>=ry2&&mouseY<=ry2+bh2;
            qFill(rx2, ry2, bw2, bh2, rH2?20:14, rH2?130:100, rH2?60:44);
            qBorder(rx2, ry2, bw2, bh2, 30, 170, 80);
            iText("Run", rx2+10, ry2+bh2*0.76f, 160, 240, 180, FST);
        }
    }

    // Scrollbar
    if ((int)exRows.size() > visRows) {
        float th  = visRows * rowH;
        float sbH = std::max(16.f, (float)visRows / exRows.size() * th);
        float sbY = ly + (float)exScroll / exRows.size() * th;
        qFill(px+pw-6, ly, 6, th, 30, 32, 44);
        qFill(px+pw-6, sbY, 6, sbH, 80, 90, 130);
    }

    // Footer hint
    iText("Tip: place .cpp sketches in examples/ -- they appear here automatically.",
          px+14, py+ph-10, 70, 75, 100, FST);
}

static void drawLibMgr() {
    qFill(0, 0, width, height, 0, 0, 0, 175);

    float pw=660, ph=460, px=(width-pw)*0.5f, py=(height-ph)*0.5f;
    qFill(px,py,pw,ph,32,33,42);
    qBorder(px,py,pw,ph,72,75,92);
    qFill(px,py,pw,32,40,42,54);
    iText("Library Manager",px+12,py+23,228,232,255,FS);

    bool xH=mouseX>=px+pw-28&&mouseX<=px+pw-8&&mouseY>=py+6&&mouseY<=py+26;
    qFill(px+pw-28,py+6,20,20,xH?200:85,50,58);
    iText("X",px+pw-21,py+21,240,242,248,FSS);

    if (!libStatus.empty()) {
        qFill(px+4,py+36,pw-8,18,26,46,26);
        iText(libStatus,px+10,py+49,128,220,128,FST);
    }

    float ly=py+58, rowH=34;
    int visLib=(int)((ph-66)/rowH);
    libScroll=std::max(0,std::min(libScroll,std::max(0,(int)libraries.size()-visLib)));
    qFill(px,ly-2,pw,20,40,42,54);
    iText("Library",px+10,ly+13,148,152,178,FST);
    iText("Description",px+180,ly+13,148,152,178,FST);
    iText("Action",px+pw-108,ly+13,148,152,178,FST);
    ly += 20;

    for (int i=0;i<visLib;i++) {
        int li=libScroll+i;
        if (li>=(int)libraries.size()) break;
        auto& lib=libraries[li];
        float ry=ly+i*rowH;
        bool hov=mouseX>=px&&mouseX<=px+pw&&mouseY>=ry&&mouseY<=ry+rowH;
        qFill(px,ry,pw,rowH,hov?38:28,hov?40:29,hov?52:38);
        qLine(px,ry+rowH-1,px+pw,ry+rowH-1,48,50,62);

        std::string nm=lib.name; textSize(FST);
        while (nm.size()>1&&textWidth(nm)>165) nm.pop_back();
        iText(nm,px+10,ry+rowH*0.72f,208,212,238,FST);

        std::string desc=lib.desc;
        while (desc.size()>1&&textWidth(desc)>pw-300) desc.pop_back();
        iText(desc,px+180,ry+rowH*0.72f,152,155,178,FST);

        float bx2=px+pw-106, by2=ry+5, bw2=98, bh2=rowH-10;
        if (lib.installed) {
            bool aH=mouseX>=bx2&&mouseX<=bx2+bw2&&mouseY>=by2&&mouseY<=by2+bh2;
            qFill(bx2,by2,bw2,bh2,aH?38:28,aH?110:88,aH?40:32);
            qBorder(bx2,by2,bw2,bh2,48,158,58);
            iText("+include",bx2+8,by2+bh2*0.72f,136,238,148,FST);
        } else if (li==installingLib) {
            qFill(bx2,by2,bw2,bh2,96,78,18);
            iText("Installing..",bx2+4,by2+bh2*0.72f,238,208,98,FST);
        } else {
            bool iH=mouseX>=bx2&&mouseX<=bx2+bw2&&mouseY>=by2&&mouseY<=by2+bh2;
            qFill(bx2,by2,bw2,bh2,iH?38:22,iH?98:68,iH?198:158);
            iText("Install",bx2+12,by2+bh2*0.72f,198,222,255,FST);
        }
    }
}

// =============================================================================
// FILE PICKER (inline)
// =============================================================================


static void drawFilePicker() {
    int cy = consoleY();
    qFill(0,cy,width,CONSOLE_H,22,22,22);
    qFill(0,cy,width,3,17,108,179);
    iText(fpSave?"Save Sketch As:":"Open Sketch:",10,cy+20,188,198,238,FSS);
    qFill(10,cy+25,width-94,26,16,17,24);
    qBorder(10,cy+25,width-94,26,17,108,179);
    fill(220,224,240); textSize(FSS); text(fpInput+"_",16,cy+42);

    float bbx=width-78, bby=cy+25, bbw=68, bbh=26;
    bool bbH=mouseX>=bbx&&mouseX<=bbx+bbw&&mouseY>=bby&&mouseY<=bby+bbh;
    qFill(bbx,bby,bbw,bbh,bbH?38:22,bbH?98:68,bbH?198:158);
    iText("Browse..",bbx+4,bby+bbh*0.76f,198,218,255,FST);

    iText("Enter=confirm  Esc=cancel  or click:",10,cy+62,108,112,142,FST);
    auto files = listSketches();
    for (int i=0;i<(int)files.size()&&i<6;i++) {
        float ry=cy+74+i*19;
        bool hov=mouseX>=10&&mouseX<=380&&mouseY>=ry&&mouseY<ry+19;
        if (hov) qFill(8,ry,374,18,30,78,158);
        iText(files[i],14,ry+14,hov?240:158,hov?244:165,hov?255:198,FST);
    }
}

// =============================================================================
// SETUP / DRAW
// =============================================================================


// =============================================================================
// EXPORT APPLICATION
// =============================================================================
// Layout mirrors Processing 4 export:
//   <sketchdir>/<sketchname>/
//     windows64/
//       <name>.exe
//       lib/          (DLLs: glfw3.dll, glew32.dll, etc.)
//       source/       (copy of .cpp source)
//     linux64/
//       <name>         (ELF binary)
//       lib/
//       source/
//     macos/
//       <name>
//       lib/
//       source/

static void exportWorker(std::string sketchDir, std::string sketchName,
                         bool doWin, bool doLin, bool doMac) {
    auto log = [&](const std::string& s) {
        std::lock_guard<std::mutex> lk(outMutex);
        outLines.push_back(s);
        outScroll = std::max(0, (int)outLines.size() - 14);
    };

    std::string baseDir = sketchDir + "/" + sketchName;
    plat_mkdir(baseDir);
    log("[Export] Output folder: " + baseDir);

    // Helper: populate a platform folder
    auto exportPlatform = [&](const std::string& platName,
                               const std::string& cxx,
                               const std::string& flags,
                               const std::string& ext) {
        std::string platDir  = baseDir + "/" + platName;
        std::string libDir   = platDir + "/lib";
        std::string srcDir   = platDir + "/source";
        plat_mkdir(platDir);
        plat_mkdir(libDir);
        plat_mkdir(srcDir);

        // Copy source file
        if (!currentFile.empty()) plat_copy_file(currentFile, srcDir + "/" + sketchName + ".cpp");
        // Copy Processing headers
        for (auto& f : {"Processing.h","Processing.cpp","Platform.h","Processing_defaults.cpp"})
            if (plat_file_exists(std::string("src/")+f))
                plat_copy_file(std::string("src/")+f, srcDir+"/"+f);

        // Compile
        std::string binPath = platDir + "/" + sketchName + ext;
        std::string cmd = cxx + " -std=c++17"
            " src/Processing.cpp"
            " src/Sketch_run.cpp"
            " src/Processing_defaults.cpp"
            " src/main.cpp"
            " -o \"" + binPath + "\""
            " " + flags +
#ifndef _WIN32
        " 2>&1" +
#endif
        "";
        log("[Export] Compiling for " + platName + "...");
        log("$ " + cmd);

#ifdef _WIN32
        auto _ep = plat_popen(cmd);
        FILE* p = _ep.f;
#else
        FILE* p = popen(cmd.c_str(), "r");
#endif
        if (!p) { log("[Export] ERROR: could not run compiler"); return; }
        char buf[1024];
        while (fgets(buf, sizeof(buf), p)) {
            std::string ln(buf);
            if (!ln.empty() && ln.back()=='\n') ln.pop_back();
            log(ln);
        }
#ifdef _WIN32
        int ret = plat_pclose(_ep);
#else
        int ret = pclose(p);
#endif
        if (ret != 0) { log("[Export] ERROR: compile failed for " + platName); return; }

        // Copy runtime DLLs / .so files from system lib paths
        // Windows: grab from MSYS2 mingw64/bin
        if (ext == ".exe") {
            std::vector<std::string> dlls = {
                "libglfw3.dll","glfw3.dll",
                "glew32.dll","libglew32.dll",
                "libgcc_s_seh-1.dll","libstdc++-6.dll","libwinpthread-1.dll"
            };
            std::vector<std::string> searchDirs = {
                "C:/msys64/mingw64/bin","C:/msys64/ucrt64/bin","."
            };
            for (auto& dll : dlls) {
                for (auto& dir : searchDirs) {
                    std::string candidate = dir + "/" + dll;
                    if (plat_file_exists(candidate)) {
                        plat_copy_file(candidate, libDir + "/" + dll);
                        log("[Export] Copied " + dll);
                        break;
                    }
                }
            }
        } else {
            // Linux/Mac: note the .so dependencies; user must have them installed
            log("[Export] Note: runtime libs (libGL, libglfw, libGLEW) must be installed on target system.");
        }

        // Copy assets (files/ folder if it exists)
        if (plat_file_exists("files")) {
            std::string dstFiles = platDir + "/files";
            plat_mkdir(dstFiles);
            plat_copy_dir("files", dstFiles);
            log("[Export] Copied files/ assets");
        }

        log("[Export] Done: " + platName + " -> " + binPath);
        exportStatus = "Exported to " + baseDir;
    };

#ifdef _WIN32
    std::string winCxx   = "x86_64-w64-mingw32-g++";
    std::string winFlags = "-lglfw3 -lglew32 -lopengl32 -lglu32 -lcomdlg32 -lshell32 -lole32 -luuid -mwindows -pthread -D_USE_MATH_DEFINES -static-libgcc -static-libstdc++";
    // On Windows host, native compile works without cross-compiler prefix:
    std::string natCxx   = "g++";
    std::string natFlags = "-lglfw3 -lglew32 -lopengl32 -lglu32 -lcomdlg32 -lshell32 -lole32 -luuid -mwindows -pthread -D_USE_MATH_DEFINES";
    std::string linCxx   = "x86_64-linux-gnu-g++";
    std::string linFlags = "-lglfw -lGLEW -lGL -lGLU -lm -pthread";
#else
    std::string natCxx   = "g++";
    std::string natFlags = "-lglfw -lGLEW -lGL -lGLU -lm -pthread";
    std::string linCxx   = "g++";
    std::string linFlags = natFlags;
    std::string winCxx   = "x86_64-w64-mingw32-g++";
    std::string winFlags = "-lglfw3 -lglew32 -lopengl32 -lglu32 -lcomdlg32 -lshell32 -lole32 -luuid -mwindows -pthread -D_USE_MATH_DEFINES";
#endif

    if (doWin) exportPlatform("windows64", (plat_file_exists("x86_64-w64-mingw32-g++")||true) ? natCxx : winCxx, natFlags, ".exe");
    if (doLin) exportPlatform("linux64",   linCxx, linFlags, "");
    if (doMac) exportPlatform("macos",     "clang++", "-framework OpenGL -lglfw -lGLEW -lm -pthread", "");

    log("[Export] All done!");
    exportStatus = "Export complete";
    exportRunning = false;
    // Open the output folder
    plat_open_folder(baseDir);
}

static void doExportApp() {
    showExportDlg = true;
    exportStatus  = "";
}

// =============================================================================
// DRAW EXPORT DIALOG
// =============================================================================
static void drawExportDlg() {
    float pw = 480, ph = 320;
    float px = (width - pw) * 0.5f, py = (height - ph) * 0.5f;
    qFill(0, 0, width, height, 0, 0, 0, 160);
    qFill(px, py, pw, ph, 30, 32, 42);
    qBorder(px, py, pw, ph, 65, 70, 100);
    qLine(px, py+38, px+pw, py+38, 55, 58, 82);
    iText("Export Application", px+14, py+26, 228, 232, 255, FS);

    // Close X
    bool xH = mouseX>=px+pw-28&&mouseX<=px+pw-8&&mouseY>=py+8&&mouseY<=py+30;
    iText("X", px+pw-22, py+24, xH?255:180, xH?100:140, xH?100:180, FSS);

    float cy = py + 54;
    iText("Target platforms:", px+16, cy+14, 180, 185, 210, FST);
    cy += 22;

    // Checkboxes
    auto checkbox = [&](float cx2, float cy2, bool& val, const std::string& label) {
        bool hov = mouseX>=cx2&&mouseX<=cx2+16&&mouseY>=cy2&&mouseY<=cy2+16;
        qBorder(cx2, cy2, 16, 16, hov?140:80, hov?180:100, hov?255:160);
        if (val) {
            qFill(cx2+2, cy2+2, 12, 12, 80, 160, 255);
            iText("✓", cx2+2, cy2+13, 255, 255, 255, FST);
        }
        iText(label, cx2+22, cy2+13, 210, 215, 235, FST);
    };
    checkbox(px+30, cy,    exportWin64,   "Windows 64-bit  (.exe + DLLs)");    cy+=30;
    checkbox(px+30, cy,    exportLinux64, "Linux 64-bit    (ELF binary)");      cy+=30;
    checkbox(px+30, cy,    exportMac,     "macOS           (requires Mac host)");cy+=30;
    cy += 8;

    // Sketch file extension note
    std::string skName = sketchBin.empty() ? "untitled" : sketchBin;
    iText("Sketch: " + skName, px+16, cy+14, 140, 145, 175, FST);  cy+=22;
    std::string outDir = (currentFile.empty() ? "." : plat_dirname(currentFile)) + "/" + skName + "/";
    iText("Output: " + outDir, px+16, cy+14, 100, 160, 100, FST);  cy+=28;

    // Status
    if (!exportStatus.empty())
        iText(exportStatus, px+16, cy+14, exportRunning?200:80, exportRunning?180:220, exportRunning?80:80, FST);

    // Export button
    float bx = px+pw-130, by2 = py+ph-46, bw = 110, bh = 30;
    bool bH   = mouseX>=bx&&mouseX<=bx+bw&&mouseY>=by2&&mouseY<=by2+bh;
    bool anyPlatform = exportWin64||exportLinux64||exportMac;
    int br = exportRunning?80:(anyPlatform?(bH?40:25):20);
    int bg = exportRunning?80:(anyPlatform?(bH?160:100):60);
    int bb = exportRunning?80:(anyPlatform?(bH?255:200):100);
    qFill(bx, by2, bw, bh, br, bg, bb, anyPlatform?255:120);
    iText(exportRunning?"Exporting...":"Export", bx+18, by2+bh*0.72f, 255, 255, 255, FST);
}

void setup() {
    _wireCallbacksFn = ideWireCallbacks;  // register IDE callbacks before run() wires them
    size(1080, 740);
    windowResizable(true);
    frameRate(60);
    windowTitle("cpp-dev IDE");

    // Load window icon
    {
        static const char* ICON_PATHS[] = {
            "files/logo.jpg","files/logo.png",
            "logo.jpg","logo.png","icon.png","icon.jpg",nullptr };
        for (int i=0; ICON_PATHS[i]; i++) {
            if (plat_file_exists(ICON_PATHS[i])) {
                setWindowIcon(loadImage(ICON_PATHS[i]));
                break;
            }
        }
    }

    checkInstalled();
    // Tree is populated when user clicks "Open" -- not on startup
    refreshExamples();  // scan examples/ on startup
    outLines.push_back("cpp-dev ready.");
    outLines.push_back("Ctrl+B build | Ctrl+R run | Ctrl+. stop | Ctrl+Shift+M serial | Ctrl+Shift+L libs");


}

// Draw an animated build progress bar overlaid at the bottom of the editor.
// Only shown while isBuilding is true.
static void drawBuildProgress() {
    float progress = buildProgress.load();
    int   bh = 4;   // bar height in pixels
    int   by = statusY() - bh;
    int   bx = sbW();
    int   bw = editorFullW();

    // Dark background track
    qFill(bx, by, bw, bh, 20, 20, 28);

    // Filled portion (animated blue)
    float filled = bw * progress;
    qFill(bx, by, filled, bh, 17, 108, 179);

    // Animated shimmer on the leading edge
    float shimX = bx + filled - 12;
    if (shimX > bx)
        qFill(shimX, by, 12, bh, 80, 180, 255, 160);

    // "Building..." label in status bar area
    iText("Building...  " + std::to_string((int)(progress*100)) + "%",
          bx + 8, by - 4, 140, 160, 220, FST);
}

void draw() {
    background(30, 30, 30);

    // Poll build completion every frame (safe -- atomics)
    if (buildDone.load()) {
        buildDone.store(false);
        buildProgress.store(0.f);

        // Jump editor to first error line if build failed
        if (hasError) {
            std::lock_guard<std::mutex> lk(outMutex);
            for (auto& ol : outLines) {
                size_t pos = ol.find("Sketch_run.cpp:");
                if (pos != std::string::npos) {
                    size_t p2 = pos + 15;
                    int ln = 0;
                    while (p2 < ol.size() && isdigit((unsigned char)ol[p2]))
                        ln = ln * 10 + (ol[p2++] - '0');
                    if (ln > 0 && ln <= (int)code.size()) {
                        curLine = ln-1; curCol = 0;
                        clamp(); ensureVis();
                    }
                    break;
                }
            }
        }

        // If Ctrl+R was pressed, launch now that build succeeded
        if (pendingRun) {
            pendingRun = false;
            if (buildSucceeded.load()) launchSketch();
            else {
                outLines.push_back("Not running -- fix errors first.");
                outScroll = std::max(0, (int)outLines.size()-10);
            }
        }
    }

    drawSidebar();
    if (!fpShow) { drawEditor(); drawStatus(); }
    drawConsole();

    // Build progress bar overlaid on editor while building
    if (isBuilding.load()) drawBuildProgress();

    if (fpShow)       drawFilePicker();
    if (showLibMgr)   drawLibMgr();
    // Debugger mode: advance to next example after sketch exits
    if (debuggerAdvance && !isBuilding.load() && !sketchRunning.load()) {
        debuggerAdvance = false;
        if (debuggerMode && !examplesList.empty()) {
            std::vector<ExampleEntry> files;
            for (auto& e : examplesList) files.push_back(e);
            std::sort(files.begin(), files.end(), [](const ExampleEntry& a, const ExampleEntry& b){ return a.path < b.path; });
            debuggerIndex = debuggerIndex % (int)files.size();
            openFile(files[debuggerIndex].path);
            pendingRun = true;
            doCompile();
        }
    }
    if (showExamples)  drawExamples();
    if (showExportDlg) drawExportDlg();
    drawToolbar();   // chrome always on top
    drawMenuBar();
}

// =============================================================================
// MOUSE
// =============================================================================

static bool   dragging      = false;
static int    clickCount    = 0;
static double lastClickTime = 0.0;

static void autoFormat() {
    pushUndo();
    int depth=0;
    for (auto& ln:code) {
        std::string t=ln;
        size_t sp=t.find_first_not_of(" \t");
        if (sp!=std::string::npos) t=t.substr(sp);
        if (!t.empty()&&t[0]=='}') depth=std::max(0,depth-1);
        ln=std::string(depth*2,' ')+t;
        for (char c:t) { if(c=='{')depth++;else if(c=='}')depth--; }
        depth=std::max(0,depth);
    }
}

static void mouseToLC(int& li, int& col) {
    float lh = lineH();
    li  = scrollTop + (int)((mouseY - editorY()) / lh);
    li  = std::max(0, std::min(li, (int)code.size()-1));
    textSize(FS);
    float tx = (float)(sbW() + GUTTER_W + 4);
    col = 0;
    for (int c = 0; c < (int)code[li].size(); c++) {
        float cw = textWidth(std::string(1, code[li][c]));
        if (tx + cw * 0.5f > mouseX) break;
        tx += cw; col = c+1;
    }
}

void mousePressed() {

    // --- Examples browser ---
    if (showExamples) {
        float pw=580,ph=480,px=(width-pw)*0.5f,py=(height-ph)*0.5f;
        if (mouseX<px||mouseX>px+pw||mouseY<py||mouseY>py+ph) { showExamples=false; return; }
        // Close X
        if (mouseX>=px+pw-34&&mouseX<=px+pw-10&&mouseY>=py+8&&mouseY<=py+30) { showExamples=false; return; }
        // Refresh
        if (mouseX>=px+pw-90&&mouseX<=px+pw-34&&mouseY>=py+8&&mouseY<=py+30) { refreshExamples(); return; }
        // Debug All -- toggle debugger mode, start from first example
        if (mouseX>=px+pw-200&&mouseX<=px+pw-100&&mouseY>=py+8&&mouseY<=py+30) {
            debuggerMode = !debuggerMode;
            if (debuggerMode && !examplesList.empty()) {
                debuggerIndex = 0;
                // Collect all file entries in order
                std::vector<ExampleEntry> files;
                for (auto& e : examplesList) files.push_back(e);
                std::sort(files.begin(), files.end(), [](const ExampleEntry& a, const ExampleEntry& b){ return a.path < b.path; });
                if (!files.empty()) {
                    debuggerIndex = 0;
                    openFile(files[0].path);
                    showExamples = false;
                    pendingRun = true;
                    doCompile();
                }
            }
            return;
        }
        // Rows
        float ly=py+44+22, rowH=38.f;
        int   vis=(int)((ph-(ly-py)-8)/rowH);
        for (int i=0;i<vis;i++) {
            int li=exScroll+i;
            if (li>=(int)exRows.size()) break;
            float ry=ly+i*rowH;
            auto& row=exRows[li];
            if (mouseY<ry||mouseY>ry+rowH) continue;
            if (row.isFolder) {
                // Toggle folder open/closed
                if (exOpenFolders.count(row.folder)) exOpenFolders.erase(row.folder);
                else exOpenFolders.insert(row.folder);
                buildExRows();
                exScroll=std::max(0,std::min(exScroll,(int)exRows.size()-vis));
                return;
            }
            float bx2=px+pw-108,by2=ry+6,bw2=50,bh2=rowH-12;
            if (mouseX>=bx2&&mouseX<=bx2+bw2&&mouseY>=by2&&mouseY<=by2+bh2) {
                openFile(row.path); showExamples=false; return;
            }
            float rx2=bx2+54;
            if (mouseX>=rx2&&mouseX<=rx2+bw2&&mouseY>=by2&&mouseY<=by2+bh2) {
                openFile(row.path); showExamples=false; pendingRun=true; doCompile(); return;
            }
        }
        return;
    }

    // --- Library manager ---
    if (showLibMgr) {
        float pw=660,ph=460,px=(width-pw)*0.5f,py=(height-ph)*0.5f;
        if (mouseX>=px+pw-28&&mouseX<=px+pw-8&&mouseY>=py+6&&mouseY<=py+26) { showLibMgr=false; return; }
        float ly=py+58+20, rowH=34;
        int visLib=(int)((ph-66)/rowH);
        for (int i=0;i<visLib;i++) {
            int li=libScroll+i; if(li>=(int)libraries.size()) break;
            auto& lib=libraries[li];
            float ry=ly+i*rowH, bx2=px+pw-106, by2=ry+5, bw2=98, bh2=rowH-10;
            if (mouseX>=bx2&&mouseX<=bx2+bw2&&mouseY>=by2&&mouseY<=by2+bh2) {

                if (lib.installed) {
                    int ins=0; for(int ci=0;ci<(int)code.size();ci++) if(code[ci].find("#include")!=std::string::npos) ins=ci+1;
                    code.insert(code.begin()+ins, lib.header); modified=true; libStatus="Added: "+lib.header;
                } else {
                    installingLib=li; libStatus="Installing "+lib.name+"...";
                    std::string cmd=lib.pkg.empty()?lib.installCmd:buildInstallCmd(lib.pkg);
                    int ret=system(cmd.c_str()); lib.installed=(ret==0);
                    if (ret==0) { checkInstalled(); libStatus="Installed: "+lib.name; if(!lib.linkFlag.empty()&&buildFlags.find(lib.linkFlag)==std::string::npos) buildFlags+=" "+lib.linkFlag; }
                    else { libStatus="Failed: "+cmd; }
                    installingLib=-1;
                }
                return;
            }
        }
        return;
    }

    // --- Sidebar ---
    if (sidebarVisible && mouseX >= SIDEBAR_W-4 && mouseX <= SIDEBAR_W+2 && mouseY >= editorY()) {
        sidebarResizing=true; sidebarResizeAnchorX=mouseX; sidebarResizeAnchorW=SIDEBAR_W; return;
    }
    if (sidebarVisible && mouseX < SIDEBAR_W && mouseY >= editorY()+52) {
        float rowH=FSS*1.7f;
        int fi=ftScroll+(int)((mouseY-(editorY()+52))/rowH);
        if (fi>=0&&fi<(int)ftEntries.size()) {
            auto& fe=ftEntries[fi];
            if (fe.isDir) { fe.expanded=!fe.expanded; populateTree(); }
            else openFile(ftRoot+"/"+fe.name);
        }
        return;
    }
    // Open folder button in sidebar
    if (sidebarVisible && mouseX>=SIDEBAR_W-60 && mouseX<=SIDEBAR_W-6 && mouseY>=editorY()+4 && mouseY<=editorY()+20) {
        std::string chosen = plat_folder_dialog(ftRoot);
        if (!chosen.empty()) { ftRoot=chosen; ftEntries.clear(); populateTree(); }
        return;
    }

    // --- File picker ---
    if (fpShow) {
        int cy=consoleY();
        float bbx=width-78,bby=cy+25,bbw=68,bbh=26;
        if (mouseX>=bbx&&mouseX<=bbx+bbw&&mouseY>=bby&&mouseY<=bby+bbh) {
            if (fpSave) doSaveAs(fpInput); else doOpen();
            fpShow=false; return;
        }
        auto files=listSketches();
        for (int i=0;i<(int)files.size()&&i<6;i++) {
            float ry=cy+74+i*19;
            if (mouseX>=10&&mouseX<=380&&mouseY>=ry&&mouseY<ry+19) {
                if (fpSave) saveFile(files[i]); else openFile(files[i]);
                fpShow=false; return;
            }
        }
        return;
    }

    // --- Menu bar ---
    if (mouseY < MENUBAR_H) {
        struct MH { std::string label; Menu id; float x=0,w=0; };
        std::vector<MH> hs={{"File",Menu::File},{"Edit",Menu::Edit},{"Sketch",Menu::Sketch},{"Tools",Menu::Tools},{"Libraries",Menu::Libraries}};
        textSize(FST); float mx2=6;
        for (auto& h:hs){h.x=mx2;h.w=textWidth(h.label)+14;mx2+=h.w+2;}
        for (auto& h:hs) if(mouseX>=h.x-2&&mouseX<=h.x+h.w){openMenu=(openMenu==h.id)?Menu::None:h.id;return;}
        openMenu=Menu::None; return;
    }

    // --- Dropdown item clicks ---
    if (openMenu != Menu::None) {
        float mx=0; std::vector<MenuItem> items;
        if      (openMenu==Menu::File)      {mx=2;   items={{"New",""},{"Open...",""},{"---",""},{"Save",""},{"Save As...",""},{"---",""},{"Show Sketch Folder",""},{"Export Application...",""},{"---",""},{"Exit",""}};}
        else if (openMenu==Menu::Edit)      {mx=42;  items={{"Undo",""},{"Redo",""},{"---",""},{"Cut",""},{"Copy",""},{"Paste",""},{"---",""},{"Select All",""},{"Duplicate Line",""},{"---",""},{"Toggle Comment",""},{"Auto Format",""}};}
        else if (openMenu==Menu::Sketch)    {mx=84;  items={{"Build",""},{"Run",""},{"Stop",""}};}
        else if (openMenu==Menu::Tools)     {mx=138; items={{"Examples",""},{"---",""},{"Auto Format",""},{"---",""},{"Increase Font",""},{"Decrease Font",""}};}
        else if (openMenu==Menu::Libraries) {mx=182; items={{"Manage Libraries...",""},{"---",""},{"Add #include",""}};}
        float my=MENUBAR_H, pw=210;
        for (int i=0;i<(int)items.size();i++) {
            float ry=my+4+i*22;
            if (mouseX>=mx&&mouseX<=mx+pw&&mouseY>=ry&&mouseY<=ry+22&&items[i].label!="---") {
                std::string lbl=items[i].label; openMenu=Menu::None;
                if      (lbl=="New")                newFile();
                else if (lbl=="Open...")            doOpen();
                else if (lbl=="Save")               doSave();
                else if (lbl=="Save As...")         doSaveAs(currentFile);
                else if (lbl=="Exit")               {stopSketch();exit_sketch();}
                else if (lbl=="Build")              doCompile();
                else if (lbl=="Examples")           { refreshExamples(); showExamples=true; }
                else if (lbl=="Run")                doRun();
                else if (lbl=="Stop")               doStop();
                else if (lbl=="Show Sketch Folder") { std::string d=currentFile.empty()?".":plat_dirname(currentFile); plat_open_folder(d); }
                else if (lbl=="Export Application...") doExportApp();
                else if (lbl=="Examples")           { refreshExamples(); showExamples=!showExamples; }
                
                else if (lbl=="Undo")         { if(!undoStack.empty()){auto&u=undoStack.back();redoStack.push_back({code,{curLine,curCol}});code=u.first;curLine=u.second.first;curCol=u.second.second;undoStack.pop_back();clearSel();clamp();ensureVis();} }
                else if (lbl=="Redo")         { if(!redoStack.empty()){auto&u=redoStack.back();undoStack.push_back({code,{curLine,curCol}});code=u.first;curLine=u.second.first;curCol=u.second.second;redoStack.pop_back();clearSel();clamp();ensureVis();} }
                else if (lbl=="Cut")          { pushUndo();setClip(getSelected());deleteSel(); }
                else if (lbl=="Copy")         { setClip(getSelected()); }
                else if (lbl=="Paste")        { std::string cb=getClipboard();if(!cb.empty()){pushUndo();if(hasSel())deleteSel();std::istringstream ss(cb);std::string ln;bool first=true;while(std::getline(ss,ln)){if(!first){code.insert(code.begin()+curLine+1,code[curLine].substr(curCol));code[curLine]=code[curLine].substr(0,curCol);curLine++;curCol=0;}code[curLine].insert(curCol,ln);curCol+=(int)ln.size();first=false;}clamp();ensureVis();} }
                else if (lbl=="Select All")   { selLine=0;selCol=0;curLine=(int)code.size()-1;curCol=(int)code.back().size(); }
                else if (lbl=="Duplicate Line"){ pushUndo();code.insert(code.begin()+curLine+1,code[curLine]);curLine++;clamp();ensureVis(); }
                else if (lbl=="Toggle Comment"){ pushUndo();auto&ln=code[curLine];if(ln.size()>=2&&ln[0]=='/'&&ln[1]=='/')ln.erase(0,2);else ln="//"+ln; }
                else if (lbl=="Auto Format")   { autoFormat(); }
                else if (lbl=="Increase Font") {FS=std::min(32.0f,FS+1);}
                else if (lbl=="Decrease Font") {FS=std::max(8.0f,FS-1);}
                else if (lbl=="Manage Libraries..."){showLibMgr=true;checkInstalled();}
                return;
            }
        }
        openMenu=Menu::None; return;
    }

    // --- Toolbar buttons ---
    int ty=MENUBAR_H; float by=ty+6, bh=TOOLBAR_H-12, bw=92;
    if (mouseY>=by&&mouseY<=by+bh) {
        if (!isBuilding.load()) {
            if (mouseX>=width-196&&mouseX<=width-196+bw) { doCompile(); return; }
            if (mouseX>=width-96 &&mouseX<=width-96+bw)  { doRun();     return; }
        }
        // Examples button
        { float epx2=(float)(width-304), epw2=80;
          if (mouseX>=epx2&&mouseX<=epx2+epw2) { refreshExamples(); showExamples=true; return; } }

    }

    // --- Console area ---
    {
        int cy=consoleY(), cx2=consoleX();

        // Resize handles
        if (terminalPos==TermPos::Bottom&&mouseY>=cy&&mouseY<=cy+4) { consoleResizing=true;consoleResizeAnchorY=mouseY;consoleResizeAnchorH=CONSOLE_H; return; }
        if (terminalPos==TermPos::Right&&mouseX>=cx2&&mouseX<=cx2+4&&mouseY>=cy&&mouseY<=cy+(height-editorY())) { termSideResizing=true;termSideAnchorX=mouseX;termSideAnchorW=TERM_SIDE_W; return; }

        // Stop button
        if (sketchRunning) {
            float sx=(float)(consoleX()+consoleW()-168), sy2=(float)(cy+3), sw2=60, sh=16;
            if (mouseX>=sx&&mouseX<=sx+sw2&&mouseY>=sy2&&mouseY<=sy2+sh) { doStop(); return; }
        }

        // Tab bar clicks (including Copy All which lives in the tab bar row)
        if (mouseY>=cy+4&&mouseY<=cy+4+TAB_H) {
            // Check Copy All first -- it sits on the right of the tab bar
            textSize(FST);
            float cbw2=68, cbx=(float)(consoleX()+consoleW()-cbw2-8);
            float cby2=(float)(consoleY()+5), cbh=(float)(TAB_H-2);
            if (mouseX>=cbx&&mouseX<=cbx+cbw2) {
                // Copy All clicked
                std::string all;
                { std::lock_guard<std::mutex> lk(outMutex);
                  for (auto& l : terminals[activeTab].lines) { all += l; all += "\n"; }
                }
                if (!all.empty()) {
                    plat_set_clipboard(all);
                    setClipboard(all);
                }
                return;
            }
            // Tab switching
            float tx=(float)(consoleX()+8);
            for (int i=0;i<(int)terminals.size();i++) {
                textSize(FST); float tw=textWidth(terminals[i].name)+20;
                if (mouseX>=tx&&mouseX<=tx+tw) { activeTab=i; consoleSelLine=-1; return; }
                tx+=tw+2;
            }
            return;
        }

        // Line click -- copy line to clipboard
        if (mouseY>=cy+4+TAB_H&&mouseY<height) {
            float lh2=FSS*1.5f;
            int vi=(int)((mouseY-(cy+4+TAB_H))/lh2);
            auto& tlines=terminals[activeTab].lines;
            auto& tscroll=terminals[activeTab].scroll;
            int li=tscroll+vi;
            if (li>=0&&li<(int)tlines.size()) {
                consoleSelLine=li;
                std::lock_guard<std::mutex> lk(outMutex);
                setClip(tlines[li]);
            }
            return;
        }
    }

    // --- Editor clicks ---
    if (mouseY>=editorY()&&mouseY<editorY()+editorH()) {
        // Minimap click
        int ex2=sbW(), ew=editorFullW();
        float mmx=(float)(ex2+ew-60-8);
        if (mouseX>=mmx&&mouseX<=mmx+60) {
            float frac=(mouseY-editorY())/(float)editorH();
            curLine=(int)(frac*code.size());
            clamp(); ensureVis(); return;
        }

        // Click count detection
        double now=(double)millis()/1000.0;
        if (now-lastClickTime<0.35) clickCount++; else clickCount=1;
        lastClickTime=now;

        int li, col; mouseToLC(li, col);
        curLine = li;

        if (clickCount >= 3) {
            // Triple-click: select whole line
            selLine=li; selCol=0; curCol=(int)code[li].size(); dragging=false;
        } else if (clickCount == 2) {
            // Double-click: select word
            auto isW=[](char c){ return isalnum((unsigned char)c)||c=='_'; };
            int wl=col, wr=col;
            while (wl>0 && isW(code[li][wl-1])) wl--;
            while (wr<(int)code[li].size() && isW(code[li][wr])) wr++;
            curCol=wr; selLine=li; selCol=wl; dragging=false;
        } else {
            // Single-click
            curCol=col; selLine=li; selCol=col; dragging=true;
        }
        clamp();
    }
}

void mouseDragged() {
    if (consoleResizing) {
        int delta=consoleResizeAnchorY-mouseY;
        CONSOLE_H=std::max(CONSOLE_H_MIN,std::min(CONSOLE_H_MAX,consoleResizeAnchorH+delta));
        return;
    }
    if (termSideResizing) {
        int delta=termSideAnchorX-mouseX;
        TERM_SIDE_W=std::max(TERM_SIDE_W_MIN,std::min(TERM_SIDE_W_MAX,termSideAnchorW+delta));
        return;
    }
    if (sidebarResizing) {
        int delta=mouseX-sidebarResizeAnchorX;
        SIDEBAR_W=std::max(SIDEBAR_W_MIN,std::min(SIDEBAR_W_MAX,sidebarResizeAnchorW+delta));
        return;
    }
    if (!dragging) return;
    // Always update selection when dragging in editor area,
    // and auto-scroll if mouse goes above or below the visible area.
    if (mouseY < editorY()) {
        // Above editor -- scroll up and extend selection to top of visible
        if (scrollTop > 0) scrollTop--;
        curLine = scrollTop; curCol = 0; clamp();
    } else if (mouseY >= editorY()+editorH()) {
        // Below editor -- scroll down and extend selection
        int maxScroll = std::max(0,(int)code.size()-visLines());
        if (scrollTop < maxScroll) scrollTop++;
        curLine = std::min(scrollTop+visLines()-1,(int)code.size()-1);
        curCol = (int)code[curLine].size(); clamp();
    } else if (mouseY>=editorY()) {
        int li, col; mouseToLC(li, col);
        curLine=li; curCol=col; clamp(); ensureVis();
    }
}

void mouseReleased() {
    consoleResizing=false; termSideResizing=false; sidebarResizing=false; dragging=false;
    if (hasSel()&&selLine==curLine&&selCol==curCol) clearSel();
}

void mouseWheel(int delta) {
    bool ctrl = isCtrlDown();
    // GLFW: positive delta = scroll up (wheel rotated away from user).
    // Negate so scroll up moves content up (decreases scroll offset).
    int d = -delta;

    if (showLibMgr)   { libScroll=std::max(0,libScroll+d); return; }
    if (showExamples) {
        int vis=(int)((480.f-90.f)/38.f);
        exScroll=std::max(0,std::min(exScroll+d,
                 std::max(0,(int)exRows.size()-vis)));
        return;
    }
    if (ctrl) { FS=std::max(8.0f,std::min(32.0f,FS-(float)d)); return; }
    if (sidebarVisible&&mouseX<SIDEBAR_W) { ftScroll=std::max(0,ftScroll+d); return; }
    if (mouseY>=editorY()&&mouseY<editorY()+editorH()) {
        scrollTop=std::max(0,std::min(scrollTop+d*3,std::max(0,(int)code.size()-visLines()))); return;
    }
    if (mouseY>=consoleY()) {
        float lh2=FSS*1.5f; int vis=std::max(1,(int)((CONSOLE_H-4-TAB_H)/lh2));
        auto& ts=terminals[activeTab].scroll; auto& tl=terminals[activeTab].lines;
        ts=std::max(0,std::min(ts+d*2,std::max(0,(int)tl.size()-vis)));
    }
}

// =============================================================================
// KEYBOARD
// =============================================================================

void keyPressed() {
    bool ctrl = isCtrlDown(), shift = isShiftDown();

    // --- File picker input ---
    if (fpShow) {
        if (key==ESC) { fpShow=false; return; }
        if ((key==ENTER||key==RETURN)&&!fpInput.empty()) { if(fpSave)saveFile(fpInput);else openFile(fpInput); fpShow=false; }
        if (key==BACKSPACE&&!fpInput.empty()) fpInput.pop_back();
        return;
    }

    // --- ESC: close modals or exit vim modes ---
    if (key==ESC) {
        if (showLibMgr||fpShow||showExamples||showExportDlg) { showLibMgr=fpShow=showExamples=showExportDlg=false; return; }
        if (openMenu!=Menu::None) { openMenu=Menu::None; return; }
        clearSel(); return;
    }

    // --- Ctrl+. stop sketch ---
    if (ctrl && keyCode==PERIOD_KEY) { doStop(); return; }

    // --- Ctrl+= / Ctrl+- font size ---
    if (ctrl && keyCode==EQUAL_KEY) { FS=std::min(32.0f,FS+1); return; }
    if (ctrl && keyCode==MINUS_KEY) { FS=std::max(8.0f,FS-1);  return; }

    // --- Ctrl+Shift+* shortcuts ---
    if (ctrl) {
        if (keyCode==KEY_N) { newFile(); return; }
        if (keyCode==KEY_O) { doOpen(); return; }
        if (keyCode==KEY_S) { if(shift) doSaveAs(currentFile); else doSave(); return; }
        if (keyCode==KEY_B) { doCompile(); return; }
        if (keyCode==KEY_R) { doRun(); return; }
        if (keyCode==KEY_L&&shift) { showLibMgr=true;checkInstalled(); return; }
        if (keyCode==KEY_E&&shift) { refreshExamples();showExamples=!showExamples; return; }
        if (keyCode==KEY_Z&&!shift) { if(!undoStack.empty()){auto&u=undoStack.back();redoStack.push_back({code,{curLine,curCol}});code=u.first;curLine=u.second.first;curCol=u.second.second;undoStack.pop_back();clearSel();clamp();ensureVis();} return; }
        if (keyCode==KEY_Y||(keyCode==KEY_Z&&shift)) { if(!redoStack.empty()){auto&u=redoStack.back();undoStack.push_back({code,{curLine,curCol}});code=u.first;curLine=u.second.first;curCol=u.second.second;redoStack.pop_back();clearSel();clamp();ensureVis();} return; }
        if (keyCode==KEY_A) { selLine=0;selCol=0;curLine=(int)code.size()-1;curCol=(int)code.back().size(); return; }
        if (keyCode==KEY_C) {
            std::string sel = getSelected();
            if (sel.empty() && curLine < (int)code.size()) {
                // No selection: copy the whole current line (like most modern editors)
                plat_set_clipboard(code[curLine] + "\n");
                setClipboard(code[curLine] + "\n");
            } else {
                setClip(sel);
            }
            return;
        }
        if (keyCode==KEY_X) { pushUndo();setClip(getSelected());deleteSel(); return; }
        if (keyCode==KEY_V) {
            std::string cbStr=getClipboard(); const char* cb=cbStr.empty()?nullptr:cbStr.c_str();
            if (cb) {
                pushUndo(); if(hasSel())deleteSel();
                std::istringstream ss(cb); std::string ln; bool first=true;
                while(std::getline(ss,ln)){if(!first){code.insert(code.begin()+curLine+1,code[curLine].substr(curCol));code[curLine]=code[curLine].substr(0,curCol);curLine++;curCol=0;}code[curLine].insert(curCol,ln);curCol+=(int)ln.size();first=false;}
                clamp();ensureVis();
            }
            return;
        }
        if (keyCode==KEY_D) { pushUndo();code.insert(code.begin()+curLine+1,code[curLine]);curLine++;clamp();ensureVis(); return; }
        if (keyCode==SLASH_KEY) { pushUndo();auto&ln=code[curLine];if(ln.size()>=2&&ln[0]=='/'&&ln[1]=='/')ln.erase(0,2);else ln="//"+ln; return; }
        if (keyCode==KEY_F&&shift) { autoFormat(); return; }
        return;
    }

    // --- Arrow / navigation keys ---
    {
        auto doAnchor=[&](){if(shift&&!hasSel()){selLine=curLine;selCol=curCol;}else if(!shift)clearSel();};
        bool nav=true;
        if      (keyCode==UP)               { doAnchor(); curLine--; }
        else if (keyCode==DOWN)             { doAnchor(); curLine++; }
        else if (keyCode==LEFT)         { doAnchor(); if(curCol>0)curCol--;else if(curLine>0){curLine--;curCol=(int)code[curLine].size();} }
        else if (keyCode==RIGHT)        { doAnchor(); if(curCol<(int)code[curLine].size())curCol++;else if(curLine<(int)code.size()-1){curLine++;curCol=0;} }
        else if (keyCode==HOME_KEY)    { doAnchor(); curCol=0; }
        else if (keyCode==END_KEY)     { doAnchor(); curCol=(int)code[curLine].size(); }
        else if (keyCode==PAGE_UP) { doAnchor(); curLine-=visLines(); }
        else if (keyCode==PAGE_DOWN){ doAnchor(); curLine+=visLines(); }
        else nav=false;
        if (nav) { clamp(); ensureVis(); return; }
    }

    // --- ENTER ---
    if (key==ENTER || key==RETURN) {
        pushUndo(); if(hasSel())deleteSel();
        std::string& cur=code[curLine];
        std::string before=cur.substr(0,curCol);
        std::string ind="";
        for (char c:before) if(isspace((unsigned char)c)) ind+=c; else ind="";
        if (!before.empty()&&before.back()=='{') ind+="  ";
        std::string after=cur.substr(curCol);
        cur=before; code.insert(code.begin()+curLine+1,ind+after);
        curLine++; curCol=(int)ind.size(); clamp(); ensureVis(); return;
    }

    // --- BACKSPACE ---
    if (key==BACKSPACE) {
        pushUndo(); if(hasSel()){deleteSel();}
        else if(curCol>0){code[curLine].erase(curCol-1,1);curCol--;}
        else if(curLine>0){int pl=(int)code[curLine-1].size();code[curLine-1]+=code[curLine];code.erase(code.begin()+curLine);curLine--;curCol=pl;}
        clamp(); ensureVis(); return;
    }

    // --- DELETE ---
    if (key==DELETE) {
        pushUndo(); if(hasSel()){deleteSel();}
        else if(curCol<(int)code[curLine].size())code[curLine].erase(curCol,1);
        else if(curLine<(int)code.size()-1){code[curLine]+=code[curLine+1];code.erase(code.begin()+curLine+1);}
        clamp(); ensureVis(); return;
    }

    // --- TAB ---
    if (key==TAB) { pushUndo();if(hasSel())deleteSel();code[curLine].insert(curCol,"  ");curCol+=2; }

    clamp(); ensureVis();
}

void keyTyped() {
    if (fpShow)     { if(key>=32&&key<127) fpInput+=key;     return; }
    if (key>=32&&key<127) {
        pushUndo(); if(hasSel())deleteSel();
        code[curLine].insert(curCol,1,key);
        curCol++; clamp(); ensureVis();
    }
}

// Forwarder bodies -- after all IDE callbacks are defined
static void _fwd_keyPressed()      { keyPressed(); }
static void _fwd_keyTyped()        { keyTyped(); }
static void _fwd_mousePressed()    { mousePressed(); }
static void _fwd_mouseDragged()    { mouseDragged(); }
static void _fwd_mouseReleased()   { mouseReleased(); }
static void _fwd_mouseWheel(int d) { mouseWheel(d); }

} // namespace Processing
