#!/usr/bin/env bash
# =============================================================================
# cpp-dev -- Linux / macOS Setup
# Run: chmod +x setup.sh && ./setup.sh
# =============================================================================
set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

R='\033[0;31m' G='\033[0;32m' Y='\033[1;33m'
B='\033[0;34m' C='\033[0;36m' N='\033[0m'
info() { echo -e "${B}[INFO]${N} $1"; }
ok()   { echo -e "${G}[ OK ]${N} $1"; }
warn() { echo -e "${Y}[WARN]${N} $1"; }
step() { echo -e "\n${C}--- $1${N}"; }
die()  { echo -e "${R}[ERR ]${N} $1"; exit 1; }

echo ""
echo -e "${C} +============================================+"
echo -e " |   cpp-dev -- Linux/macOS Setup      |"
echo -e " +============================================+${N}"
echo ""

# --- Detect distro ---------------------------------------------------------
step "Detecting platform"
PLAT=""
if [[ "$OSTYPE" == "darwin"* ]]; then
    PLAT=macos; info "macOS"
elif command -v pacman  &>/dev/null; then PLAT=arch;     info "Linux -- Arch (pacman)"
elif command -v apt-get &>/dev/null; then PLAT=debian;   info "Linux -- Debian/Ubuntu (apt)"
elif command -v dnf     &>/dev/null; then PLAT=fedora;   info "Linux -- Fedora (dnf)"
elif command -v zypper  &>/dev/null; then PLAT=opensuse; info "Linux -- openSUSE (zypper)"
else die "Unknown distro. Install g++, libglfw, libGLEW, libGL manually then run: bash buildIDE.sh"; fi

# --- Install system packages -----------------------------------------------
step "Installing dependencies"
case "$PLAT" in
    macos)
        command -v brew &>/dev/null || die "Homebrew not found. Install from https://brew.sh"
        brew install glew glfw || warn "brew errors -- packages may already be installed"
        ;;
    arch)
        sudo pacman -S --needed --noconfirm \
            base-devel glew mesa glu \
            glfw-x11 ttf-dejavu zenity xdg-utils libserialport 2>/dev/null \
        || sudo pacman -S --needed --noconfirm \
            base-devel glew mesa glu \
            glfw-wayland ttf-dejavu zenity xdg-utils libserialport \
        || warn "pacman errors -- some packages may already be installed"
        ;;
    debian)
        sudo apt-get update -q
        sudo apt-get install -y \
            build-essential libglfw3-dev libglew-dev \
            libglu1-mesa-dev mesa-common-dev \
            fonts-dejavu zenity xdg-utils libserialport-dev
        ;;
    fedora)
        sudo dnf install -y \
            gcc-c++ make glfw-devel glew-devel mesa-libGLU-devel \
            dejavu-fonts-common zenity xdg-utils libserialport-devel
        ;;
    opensuse)
        sudo zypper install -y \
            gcc-c++ make libglfw-devel glew-devel glu-devel \
            dejavu-fonts zenity xdg-utils libserialport-devel
        ;;
esac
ok "Dependencies installed"

# --- Download stb headers --------------------------------------------------
step "Downloading headers"
mkdir -p src
dl() {
    local url="$1" dest="$2" name="$3"
    [ -f "$dest" ] && { ok "$name already present"; return; }
    info "Downloading $name..."
    if command -v curl &>/dev/null; then curl -sL "$url" -o "$dest"
    elif command -v wget &>/dev/null; then wget -q "$url" -O "$dest"
    else warn "curl/wget not found -- $name skipped"; return; fi
    [ -f "$dest" ] && ok "$name" || warn "$name download failed"
}
dl "https://raw.githubusercontent.com/nothings/stb/master/stb_truetype.h" src/stb_truetype.h "stb_truetype.h"
dl "https://raw.githubusercontent.com/nothings/stb/master/stb_image.h"    src/stb_image.h    "stb_image.h"

# --- Font ------------------------------------------------------------------
if [ ! -f default.ttf ]; then
    FONT=""
    if [[ $PLAT == macos ]]; then
        for f in /Library/Fonts/Arial.ttf /System/Library/Fonts/Supplemental/Arial.ttf; do
            [ -f "$f" ] && { FONT="$f"; break; }
        done
    else
        for f in \
            /usr/share/fonts/TTF/DejaVuSansMono.ttf \
            /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf \
            /usr/share/fonts/dejavu/DejaVuSansMono.ttf \
            /usr/share/fonts/TTF/DejaVuSans.ttf \
            /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf \
            /usr/share/fonts/noto/NotoSans-Regular.ttf \
            /usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf; do
            [ -f "$f" ] && { FONT="$f"; break; }
        done
    fi
    if [ -n "$FONT" ]; then cp "$FONT" default.ttf && ok "Font: $(basename "$FONT") -> default.ttf"
    else warn "No font found -- place any .ttf in project root as default.ttf"; fi
else
    ok "default.ttf already present"
fi

# --- Create project folder structure ----------------------------------------
step "Setting up project folders"
mkdir -p files lib

# Place logo.jpg in files/ if it exists at root
if [ -f "logo.jpg" ] && [ ! -f "files/logo.jpg" ]; then
    cp logo.jpg files/logo.jpg && ok "logo.jpg -> files/logo.jpg"
elif [ -f "files/logo.jpg" ]; then
    ok "files/logo.jpg already present"
fi


# Copy sample sketches into files/ if they exist at root
for SAMPLE in Geometry.cpp Mixture.cpp Mandelbrot.cpp StoringInput.cpp; do
    if [ -f "$SAMPLE" ] && [ ! -f "files/$SAMPLE" ]; then
        cp "$SAMPLE" "files/$SAMPLE" && ok "Sample: $SAMPLE -> files/"
    fi
done

# Create examples/ folder and populate with sample sketches
mkdir -p examples
for SAMPLE in Geometry.cpp Mixture.cpp Mandelbrot.cpp StoringInput.cpp; do
    if [ -f "$SAMPLE" ] && [ ! -f "examples/$SAMPLE" ]; then
        cp "$SAMPLE" "examples/$SAMPLE" && ok "Example: $SAMPLE -> examples/"
    elif [ -f "examples/$SAMPLE" ]; then
        ok "examples/$SAMPLE already present"
    fi
done
ok "Project folders ready (files/ lib/ examples/)"

# --- src/main.cpp ----------------------------------------------------------
if [ ! -f src/main.cpp ]; then
    cat > src/main.cpp << 'MAINCPP'
#include "Processing.h"
#include <string>
int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++)
        if (std::string(argv[i]) == "--debug")
            Processing::enableDebugConsole();
    Processing::run();
    return 0;
}
MAINCPP
    ok "src/main.cpp written"
else
    ok "src/main.cpp already present"
fi

# --- Write build scripts ---------------------------------------------------
step "Writing build scripts"

if [[ $PLAT == macos ]]; then
    LD="-lglfw -lGLEW -framework OpenGL -framework Cocoa -framework IOKit -lm"
else
    LD="-lglfw -lGLEW -lGL -lGLU -lm -pthread"
fi

cat > buildIDE.sh << BIDE
#!/usr/bin/env bash
set -e
echo "[build] Compiling IDE..."
g++ -std=c++17 \\
    src/Processing.cpp \\
    src/IDE.cpp \\
    src/main.cpp \\
    -o cpp-dev \\
    $LD
echo "[build] Done: ./cpp-dev"
BIDE
chmod +x buildIDE.sh; ok "buildIDE.sh"

cat > build.sh << BUILD
#!/usr/bin/env bash
set -e
SKETCH="\${1:-src/MySketch.cpp}"
OUT="\${2:-SketchApp}"
echo "[build] \$SKETCH -> \$OUT"
g++ -std=c++17 \\
    src/Processing.cpp \\
    "\$SKETCH" \\
    src/main.cpp \\
    -o "\$OUT" \\
    $LD
echo "[build] Done: ./\$OUT"
BUILD
chmod +x build.sh; ok "build.sh"

cat > run.sh << 'RUN'
#!/usr/bin/env bash
set -e
bash build.sh "${1:-src/MySketch.cpp}" "${2:-SketchApp}"
./"${2:-SketchApp}"
RUN
chmod +x run.sh; ok "run.sh"

# --- Check source files ----------------------------------------------------
step "Checking source files"
MISSING=0
for f in src/Processing.h src/Processing.cpp src/Platform.h src/IDE.cpp; do
    if [ ! -f "$f" ]; then warn "Missing: $f"; MISSING=$((MISSING+1))
    else ok "Found: $f"; fi
done
[ $MISSING -gt 0 ] && die "Copy missing files into src/ then re-run ./setup.sh"

# --- Build IDE -------------------------------------------------------------
step "Building IDE"
bash buildIDE.sh

# --- Optional AppImage (Arch only) -----------------------------------------
if [[ $PLAT == arch ]]; then
    echo ""
    read -r -p "  Package as portable AppImage? [y/N] " ans
    if [[ "$ans" =~ ^[Yy]$ ]]; then
        info "Building AppImage..."
        g++ -std=c++17 src/Processing.cpp src/IDE.cpp src/main.cpp \
            -o ide_appimage $LD -O2 -static-libgcc -static-libstdc++ \
        || g++ -std=c++17 src/Processing.cpp src/IDE.cpp src/main.cpp \
            -o ide_appimage $LD -O2

        APP="cpp-dev"; APPDIR="$SCRIPT_DIR/${APP}.AppDir"
        rm -rf "$APPDIR"; mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/share/cpp-dev"
        cp ide_appimage "$APPDIR/usr/bin/ide"
        [ -f default.ttf ] && cp default.ttf "$APPDIR/usr/share/cpp-dev/"
        for h in src/stb_truetype.h src/stb_image.h; do [ -f "$h" ] && cp "$h" "$APPDIR/usr/share/cpp-dev/"; done

        cat > "$APPDIR/AppRun" << 'AR'
#!/usr/bin/env bash
HERE="$(dirname "$(readlink -f "$0")")"
DIR="$HOME/cpp-dev"; mkdir -p "$DIR/src"
for f in stb_truetype.h stb_image.h; do
    [ -f "$DIR/src/$f" ] || cp "$HERE/usr/share/cpp-dev/$f" "$DIR/src/$f" 2>/dev/null || true
done
[ -f "$DIR/default.ttf" ] || cp "$HERE/usr/share/cpp-dev/default.ttf" "$DIR/default.ttf" 2>/dev/null || true
[ -f "$DIR/src/main.cpp" ] || printf '#include "Processing.h"\nint main(){Processing::run();return 0;}\n' > "$DIR/src/main.cpp"
cd "$DIR"; exec "$HERE/usr/bin/ide" "$@"
AR
        chmod +x "$APPDIR/AppRun"
        printf '[Desktop Entry]\nName=cpp-dev IDE\nExec=ide\nIcon=cpp-dev\nType=Application\nCategories=Development;\n' > "$APPDIR/cpp-dev.desktop"
        touch "$APPDIR/cpp-dev.png"

        if ! command -v appimagetool &>/dev/null; then
            curl -sL https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage \
                -o /tmp/appimagetool && chmod +x /tmp/appimagetool
            TOOL=/tmp/appimagetool
        else TOOL=appimagetool; fi

        ARCH=x86_64 "$TOOL" "$APPDIR" "${APP}-x86_64.AppImage"
        rm -f ide_appimage
        ok "Created: ${APP}-x86_64.AppImage"
    fi
fi

# --- Done ------------------------------------------------------------------
echo ""
echo -e "${G} +==========================================+"
echo -e " |   Setup complete! Launching IDE...       |"
echo -e " +==========================================+${N}"
echo ""
echo -e "  ${C}Ctrl+B${N}  build sketch      ${C}Ctrl+R${N}  build+run"
echo -e "  ${C}Ctrl+.${N}  stop sketch       ${C}Ctrl+S${N}  save"
echo -e "  ${C}Ctrl+Shift+M${N}  serial monitor"
echo -e "  ${C}Ctrl+Shift+L${N}  library manager"
echo -e "  ${C}Ctrl+Shift+V${N}  vim mode"
echo ""
exec ./cpp-dev
