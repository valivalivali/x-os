#!/bin/bash
# Minimal QEMU build for Apple Silicon with virgl/ANGLE GPU rendering.
#
# No recursive submodules needed — QEMU's meson build system handles
# firmware/ROM downloads differently. We just clone, patch, configure, build.
#
# Usage:
#   ./tools/build-qemu-virgl.sh           # full build
#   ./tools/build-qemu-virgl.sh rebuild   # rebuild after source edits (no re-clone)
#   ./tools/build-qemu-virgl.sh clean     # clean build dir

set -euo pipefail

QEMU_VERSION="v11.0.2"
PATCH_URL="https://raw.githubusercontent.com/startergo/homebrew-qemu-virgl/refs/heads/master/Patches/qemu-v07.diff"

BUILD_DIR="${BUILD_DIR:-/opt/qemu-build}"
INSTALL_PREFIX="${INSTALL_PREFIX:-/opt/qemu-head}"
JOBS=$(sysctl -n hw.ncpu)

# Homebrew prefixes for virgl stack (already installed from startergo tap)
ANGLE_PREFIX="$(brew --prefix libangle)"
EPOXY_PREFIX="$(brew --prefix libepoxy-angle)"
VIRGL_PREFIX="$(brew --prefix virglrenderer)"
SPICE_PROTO_PREFIX="$(brew --prefix spice-protocol)"
SPICE_SERVER_PREFIX="$(brew --prefix spice-server)"

echo "=== QEMU virgl build for Apple Silicon ==="
echo "  Version:     ${QEMU_VERSION}"
echo "  Build dir:   ${BUILD_DIR}"
echo "  Install to:  ${INSTALL_PREFIX}"
echo "  Jobs:        ${JOBS}"
echo ""

case "${1:-build}" in
    clean)
        echo "Cleaning build directory..."
        rm -rf "${BUILD_DIR}"
        echo "Done."
        exit 0
        ;;
esac

# Step 1: Clone QEMU (NO recursive submodules — not needed for build)
if [ ! -d "${BUILD_DIR}/qemu" ]; then
    mkdir -p "${BUILD_DIR}"
    cd "${BUILD_DIR}"
    echo "=== Cloning QEMU ${QEMU_VERSION} (no submodules) ==="
    git clone --depth 1 --branch "${QEMU_VERSION}" https://gitlab.com/qemu-project/qemu.git
    cd qemu
else
    echo "=== Using existing clone at ${BUILD_DIR}/qemu ==="
    cd "${BUILD_DIR}/qemu"
fi

# Step 2: Apply the startergo patch for macOS Cocoa + virgl + ANGLE
echo "=== Applying virgl/ANGLE/Cocoa patch ==="
PATCH_FILE="${BUILD_DIR}/qemu-v07.diff"
if [ ! -f "${PATCH_FILE}" ]; then
    curl -sL "${PATCH_URL}" -o "${PATCH_FILE}"
fi
if git diff --quiet HEAD 2>/dev/null; then
    git apply --check "${PATCH_FILE}" 2>/dev/null && {
        git apply "${PATCH_FILE}"
        echo "  Patch applied successfully."
    } || {
        echo "  WARNING: Patch didn't apply cleanly, trying with --reject..."
        git apply --reject --whitespace=fix "${PATCH_FILE}" || true
        echo "  Check .rej files manually if needed. Continuing..."
    }
else
    echo "  Patch already applied, skipping."
fi

# Step 2b: Fix patch incompatibilities with QEMU v11 API changes
echo "=== Fixing patch incompatibilities with QEMU v11 ==="

# Fix 1: egl-helpers.c — patch merged qemu_egl_init_dpy_platform into
# qemu_egl_get_display incorrectly. Restore qemu_egl_get_display to just
# return the EGLDisplay, and add qemu_egl_init_dpy_platform as a wrapper.
EGL_HELPERS="${BUILD_DIR}/qemu/ui/egl-helpers.c"
if grep -q 'return qemu_egl_init_dpy(dpy, mode);' "${EGL_HELPERS}" 2>/dev/null; then
    python3 -c "
import re
with open('${EGL_HELPERS}', 'r') as f:
    c = f.read()
old = '''    if (dpy == EGL_NO_DISPLAY) {
        error_report(\"egl: eglGetDisplay failed\");
        return -1;
    }

    return qemu_egl_init_dpy(dpy, mode);
}

#endif

#if defined(CONFIG_X11) || defined(CONFIG_GBM)
int qemu_egl_init_dpy_x11(EGLNativeDisplayType dpy, DisplayGLMode mode)'''
new = '''    if (dpy == EGL_NO_DISPLAY) {
        error_report(\"egl: eglGetDisplay failed\");
        return EGL_NO_DISPLAY;
    }

    return dpy;
}

#endif

#if defined(CONFIG_X11) || defined(CONFIG_GBM)
static int qemu_egl_init_dpy_platform(EGLNativeDisplayType dpy,
                                       EGLenum platform,
                                       DisplayGLMode mode)
{
    EGLDisplay edpy = qemu_egl_get_display(dpy, platform);
    if (edpy == EGL_NO_DISPLAY) {
        return -1;
    }
    return qemu_egl_init_dpy(edpy, mode);
}

int qemu_egl_init_dpy_x11(EGLNativeDisplayType dpy, DisplayGLMode mode)'''
c = c.replace(old, new)
with open('${EGL_HELPERS}', 'w') as f:
    f.write(c)
print('  Fixed qemu_egl_get_display / qemu_egl_init_dpy_platform')
"
fi

# Fix 2: egl-helpers.c — cast EGLNativeDisplayType (int on macOS) to void*
# for eglGetPlatformDisplayEXT
if grep -q 'eglGetPlatformDisplayEXT(platform, native, NULL)' "${EGL_HELPERS}" 2>/dev/null; then
    sed -i '' 's/eglGetPlatformDisplayEXT(platform, native, NULL)/eglGetPlatformDisplayEXT(platform, (void *)(intptr_t)native, NULL)/' "${EGL_HELPERS}"
    echo "  Fixed EGLNativeDisplayType cast for eglGetPlatformDisplayEXT"
fi

# Fix 3: egl-helpers.h — add missing declarations for Cocoa EGL functions
EGL_HELPERS_H="${BUILD_DIR}/qemu/include/ui/egl-helpers.h"
if ! grep -q 'qemu_egl_init_dpy_cocoa' "${EGL_HELPERS_H}" 2>/dev/null; then
    sed -i '' '/^#endif \/\* EGL_HELPERS_H \*\//i\
\
EGLSurface qemu_egl_init_surface(EGLContext ectx, EGLNativeWindowType win);\
int qemu_egl_init_dpy_cocoa(DisplayGLMode mode);\
' "${EGL_HELPERS_H}"
    echo "  Added missing EGL helper declarations to header"
fi

# Fix 4: cocoa.m — qemu_egl_create_context now takes 3 args (added share_context)
COCOA_M="${BUILD_DIR}/qemu/ui/cocoa.m"
if grep -q 'qemu_egl_create_context(dgc, params);' "${COCOA_M}" 2>/dev/null; then
    sed -i '' 's/qemu_egl_create_context(dgc, params);/qemu_egl_create_context(dgc, params, EGL_NO_CONTEXT);/' "${COCOA_M}"
    echo "  Fixed qemu_egl_create_context call signature (3 args)"
fi

# Step 3: Set up Python venv (QEMU needs tomli for configure)
echo "=== Setting up Python venv ==="
PYTHON3="$(which python3)"
PY_VERSION=$("${PYTHON3}" -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')
if [ ! -d "${BUILD_DIR}/venv" ]; then
    "${PYTHON3}" -m venv "${BUILD_DIR}/venv"
    "${BUILD_DIR}/venv/bin/python" -m pip install --quiet tomli
fi
export PYTHON="${BUILD_DIR}/venv/bin/python"
export PYTHONPATH="${BUILD_DIR}/venv/lib/python${PY_VERSION}/site-packages:${PYTHONPATH:-}"
export LIBTOOL="glibtool"

# Use ANGLE libepoxy instead of the regular one (which lacks EGL symbols)
export PKG_CONFIG_PATH="${EPOXY_PREFIX}/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
export LDFLAGS="-L/opt/homebrew/lib ${LDFLAGS:-}"
export CFLAGS="-I/opt/homebrew/include ${CFLAGS:-}"

# Step 4: Configure — these flags activate virgl + Cocoa + OpenGL on Apple Silicon
echo "=== Configuring QEMU ==="
./configure \
    --prefix="${INSTALL_PREFIX}" \
    --cc=cc \
    --host-cc=cc \
    --disable-bsd-user \
    --disable-guest-agent \
    --disable-sdl \
    --disable-gtk \
    --enable-cocoa \
    --enable-opengl \
    --enable-virglrenderer \
    --enable-curses \
    --enable-libssh \
    --enable-slirp \
    --enable-vde \
    --enable-fdt=system \
    --enable-debug \
    --enable-debug-info \
    --enable-trace-backends=log,simple \
    --enable-malloc=system \
    --extra-cflags="-I/opt/homebrew/include" \
    --extra-cflags="-I${ANGLE_PREFIX}/include" \
    --extra-cflags="-I${EPOXY_PREFIX}/include" \
    --extra-cflags="-I${VIRGL_PREFIX}/include" \
    --extra-cflags="-I${SPICE_PROTO_PREFIX}/include/spice-1" \
    --extra-cflags="-I${SPICE_SERVER_PREFIX}/include/spice-server" \
    --extra-cflags="-DNCURSES_WIDECHAR=1" \
    --extra-ldflags="-L/opt/homebrew/lib" \
    --extra-ldflags="-L${ANGLE_PREFIX}/lib" \
    --extra-ldflags="-L${EPOXY_PREFIX}/lib" \
    --extra-ldflags="-L${VIRGL_PREFIX}/lib" \
    --extra-ldflags="-L${SPICE_SERVER_PREFIX}/lib" \
    --extra-ldflags="-Wl,-rpath,${ANGLE_PREFIX}/lib" \
    --extra-ldflags="-Wl,-rpath,${EPOXY_PREFIX}/lib" \
    --extra-ldflags="-Wl,-rpath,${VIRGL_PREFIX}/lib" \
    --extra-ldflags="-Wl,-rpath,${SPICE_SERVER_PREFIX}/lib" \
    --target-list=aarch64-softmmu,x86_64-softmmu,i386-softmmu

# Step 5: Build
echo "=== Building QEMU (${JOBS} jobs) ==="
make V=1 -j"${JOBS}"

# Step 6: Install
echo "=== Installing to ${INSTALL_PREFIX} ==="
sudo make install

# Step 7: Fix dylib paths so binaries find ANGLE/virgl without DYLD_LIBRARY_PATH
echo "=== Fixing dynamic library paths ==="
for binary in "${INSTALL_PREFIX}/bin"/qemu-*; do
    [ -x "${binary}" ] && [ ! -L "${binary}" ] || continue
    # Clear extended attrs (resource fork, Finder info) that break codesign
    sudo xattr -cr "${binary}" 2>/dev/null || true
    sudo install_name_tool -add_rpath "${ANGLE_PREFIX}/lib" "${binary}" 2>/dev/null || true
    sudo install_name_tool -add_rpath "${EPOXY_PREFIX}/lib" "${binary}" 2>/dev/null || true
    sudo install_name_tool -add_rpath "${VIRGL_PREFIX}/lib" "${binary}" 2>/dev/null || true
    sudo install_name_tool -add_rpath "${SPICE_SERVER_PREFIX}/lib" "${binary}" 2>/dev/null || true
    for lib in libEGL.dylib libGLESv2.dylib; do
        sudo install_name_tool -change "${ANGLE_PREFIX}/lib/${lib}" "@rpath/${lib}" "${binary}" 2>/dev/null || true
    done
    sudo install_name_tool -change "${EPOXY_PREFIX}/lib/libepoxy.0.dylib" "@rpath/libepoxy.0.dylib" "${binary}" 2>/dev/null || true
    sudo install_name_tool -change "${VIRGL_PREFIX}/lib/libvirglrenderer.1.dylib" "@rpath/libvirglrenderer.1.dylib" "${binary}" 2>/dev/null || true
done

# Step 8: Code-sign binaries with HVF entitlements
echo "=== Code-signing with HVF entitlements ==="
ENTITLEMENTS="${BUILD_DIR}/qemu/accel/hvf/entitlements.plist"
if [ -f "${ENTITLEMENTS}" ]; then
    for binary in "${INSTALL_PREFIX}/bin"/qemu-system-*; do
        [ -x "${binary}" ] && [ ! -L "${binary}" ] || continue
        sudo xattr -cr "${binary}" 2>/dev/null || true
        sudo codesign --sign - --entitlements "${ENTITLEMENTS}" --force "${binary}" 2>/dev/null && echo "  Signed: $(basename ${binary})" || echo "  WARNING: Failed to sign $(basename ${binary})"
    done
fi

echo ""
echo "=== Build complete! ==="
echo "  ${INSTALL_PREFIX}/bin/qemu-system-x86_64 --version"
echo "  ${INSTALL_PREFIX}/bin/qemu-system-x86_64 -accel help"
echo ""
echo "  Your Makefile already uses /opt/qemu-head/bin/qemu-system-x86_64 first."
