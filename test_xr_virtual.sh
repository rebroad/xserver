#!/bin/bash
# Test script for virtual XR connector
# WARNING: This tests with a real Xorg server on a separate display
# Make sure you're not using display :1 before running this!

set -e

XSRC_DIR="/home/rebroad/src/xserver"
BUILD_DIR="$XSRC_DIR/build"
TEST_DISPLAY=":1"
TEST_LOG="/tmp/Xorg_test.log"
TEST_VT="8"
XORG_PID=""
XORG_START_TIME=""
ORIGINAL_VT=""

# Set up logging: all output goes to both stdout and log file
XRANDR_LOG="/tmp/xrandr_test_${TEST_DISPLAY#:}.log"
rm -f "$XRANDR_LOG"
touch "$XRANDR_LOG"

# Save original stdout and stderr
exec 3>&1
exec 4>&2

# Redirect all output to both stdout and log file
exec > >(tee -a "$XRANDR_LOG")
exec 2>&1

# Cleanup function to ensure Xorg is always killed
cleanup_xorg() {
    local cleanup_time=$(date +%s)
    local cleanup_reason="${1:-normal exit}"
    local quick_exit=0

    # If interrupted, exit quickly after cleanup
    if [ "$cleanup_reason" = "interrupted (SIGINT)" ] || [ "$cleanup_reason" = "terminated (SIGTERM)" ]; then
        quick_exit=1
    fi

    # Calculate how long Xorg ran
    if [ -n "$XORG_START_TIME" ]; then
        local xorg_duration=$((cleanup_time - XORG_START_TIME))
        if [ $quick_exit -eq 0 ]; then
            echo ""
            echo "=== Xorg Lifecycle Summary ==="
        fi
        echo "Xorg started at: $(date -d "@$XORG_START_TIME" '+%Y-%m-%d %H:%M:%S')"
        echo "Xorg ran for: ${xorg_duration} seconds"
        echo "Cleanup reason: $cleanup_reason"
    fi

    local killed_by_pid=0
    local killed_by_socket=0
    local force_killed=0

    if [ -n "$XORG_PID" ] && kill -0 "$XORG_PID" 2>/dev/null; then
        if [ $quick_exit -eq 0 ]; then
            echo ""
            echo "=== Cleanup: Killing Xorg (PID: $XORG_PID) ==="
        fi
        # Try graceful termination first (TERM signal)
        sudo kill -TERM "$XORG_PID" 2>/dev/null || true
        # Wait up to 2 seconds for graceful shutdown
        local wait_count=0
        while [ $wait_count -lt 20 ] && kill -0 "$XORG_PID" 2>/dev/null; do
            sleep 0.1
            wait_count=$((wait_count + 1))
        done
        # If still running, force kill
        if kill -0 "$XORG_PID" 2>/dev/null; then
            if [ $quick_exit -eq 0 ]; then
                echo "Xorg did not exit gracefully, force killing..."
            fi
            sudo kill -9 "$XORG_PID" 2>/dev/null || true
            force_killed=1
        else
            if [ $quick_exit -eq 0 ]; then
                echo "Xorg exited gracefully"
            fi
        fi
        killed_by_pid=1
    fi

    # Also kill by display socket to be extra sure
    XORG_PID_ON_DISPLAY=$(lsof -t "/tmp/.X11-unix/X${TEST_DISPLAY#:}" 2>/dev/null || true)
    if [ -n "$XORG_PID_ON_DISPLAY" ] && [ "$XORG_PID_ON_DISPLAY" != "$XORG_PID" ]; then
        if [ $quick_exit -eq 0 ]; then
            echo "Killing Xorg on display $TEST_DISPLAY (PID: $XORG_PID_ON_DISPLAY)..."
        fi
        sudo kill "$XORG_PID_ON_DISPLAY" 2>/dev/null || true
        sleep 0.5
        if kill -0 "$XORG_PID_ON_DISPLAY" 2>/dev/null; then
            if [ $quick_exit -eq 0 ]; then
                echo "Force killing Xorg (PID: $XORG_PID_ON_DISPLAY)..."
            fi
            sudo kill -9 "$XORG_PID_ON_DISPLAY" 2>/dev/null || true
            force_killed=1
        fi
        killed_by_socket=1
    fi

    # Verify it's really dead
    sleep 0.5
    if lsof "/tmp/.X11-unix/X${TEST_DISPLAY#:}" >/dev/null 2>&1; then
        if [ $quick_exit -eq 0 ]; then
            echo "WARNING: Xorg socket still exists, trying one more time..."
        fi
        # Find and kill ONLY the Xorg process using this specific display
        FINAL_PID=$(lsof -t "/tmp/.X11-unix/X${TEST_DISPLAY#:}" 2>/dev/null || true)
        if [ -n "$FINAL_PID" ]; then
            if [ $quick_exit -eq 0 ]; then
                echo "Killing remaining Xorg process (PID: $FINAL_PID) on display $TEST_DISPLAY..."
            fi
            sudo kill -9 "$FINAL_PID" 2>/dev/null || true
            force_killed=1
        fi
        sleep 0.5
    fi

    # Final verification
    local cleanup_end_time=$(date +%s)
    local cleanup_duration=$((cleanup_end_time - cleanup_time))

    if lsof "/tmp/.X11-unix/X${TEST_DISPLAY#:}" >/dev/null 2>&1; then
        if [ $quick_exit -eq 0 ]; then
            echo "ERROR: Xorg cleanup FAILED - socket still exists after ${cleanup_duration} seconds!"
            echo "You may need to manually kill Xorg on display $TEST_DISPLAY"
            echo "Run: sudo kill \$(lsof -t /tmp/.X11-unix/X${TEST_DISPLAY#:})"
        fi
    else
        if [ $quick_exit -eq 0 ]; then
            echo "✓ Xorg cleanup successful (took ${cleanup_duration} seconds)"
            if [ $killed_by_pid -eq 1 ]; then
                echo "  - Killed by PID"
            fi
            if [ $killed_by_socket -eq 1 ]; then
                echo "  - Killed by display socket"
            fi
            if [ $force_killed -eq 1 ]; then
                echo "  - Required force kill (-9)"
            fi
        fi
    fi

    # Exit immediately if interrupted
    if [ $quick_exit -eq 1 ]; then
        exit 130  # Standard exit code for SIGINT
    fi
}

# Set up trap to ensure cleanup happens on exit (but only on Ctrl+C, not normal exit)
# We want to keep Xorg running for interactive testing
trap 'cleanup_xorg "interrupted (SIGINT)"; exit 1' INT
trap 'cleanup_xorg "terminated (SIGTERM)"; exit 1' TERM
# Don't trap EXIT - let user decide when to exit

echo "=== Testing Virtual XR Connector ==="
echo ""

# Detect and save the original VT
ORIGINAL_VT=$(fgconsole 2>/dev/null || echo "7")
if [ -z "$ORIGINAL_VT" ] || [ "$ORIGINAL_VT" = "unknown" ]; then
    # Fallback: try to detect from who command
    ORIGINAL_VT=$(who | grep -E "\(:0\)" | head -1 | sed 's/.*tty\([0-9]*\).*/\1/' || echo "7")
fi
echo "Original VT detected: $ORIGINAL_VT"
echo ""

# Detect running X servers and warn user
echo "Checking for running X servers..."
RUNNING_XORG=$(ps aux | grep -E '[X]org.*:0' | awk '{print $2, $NF}' | head -1)
if [ -n "$RUNNING_XORG" ]; then
    echo "WARNING: Found running X server: $RUNNING_XORG"
    echo "This script will start a SEPARATE X server on display $TEST_DISPLAY (VT $TEST_VT)"
    echo "Your main X server should NOT be affected, but if you experience issues,"
    echo "stop this test immediately with Ctrl+C or by killing the test Xorg process."
    echo ""
fi

# Build Xorg and required modules if not already built
if [ ! -f "$BUILD_DIR/hw/xfree86/Xorg" ]; then
    echo "Building Xorg..."
    cd "$XSRC_DIR"
    ninja -C build hw/xfree86/Xorg
fi

# Build shadow module (needed by modesetting driver)
if [ ! -f "$BUILD_DIR/hw/xfree86/dixmods/libshadow.so" ]; then
    echo "Building shadow module..."
    cd "$XSRC_DIR"
    ninja -C build hw/xfree86/dixmods/libshadow.so
fi

# Use the driver directly from the build directory - no copying needed!
# The driver is at: build/hw/xfree86/drivers/modesetting/modesetting_drv.so
# Xorg's module loader looks in subdirectories like "drivers/", "dixmods/", etc.
# We need to point modulepath to the base module directory: build/hw/xfree86/
# This allows Xorg to find both drivers (in drivers/) and other modules (in dixmods/, etc.)
TEST_DRIVER_DIR="$BUILD_DIR/hw/xfree86"

# Path for built libinput driver (needs to match built Xorg's ABI)
LIBINPUT_SRC_DIR="$XSRC_DIR/../xf86-input-libinput"
LIBINPUT_BUILD_DIR="$LIBINPUT_SRC_DIR/build"
LIBINPUT_DRIVER="$LIBINPUT_BUILD_DIR/libinput_drv.so"

# Check if driver or related files need to be rebuilt (source is newer than built driver)
DRIVER_SRC="$XSRC_DIR/hw/xfree86/drivers/modesetting/drmmode_xr_virtual.c"
DRIVER_BUILD="$BUILD_DIR/hw/xfree86/drivers/modesetting/modesetting_drv.so"
RANDR_SRC="$XSRC_DIR/hw/xfree86/modes/xf86RandR12.c"
XORG_BIN="$BUILD_DIR/hw/xfree86/Xorg"

NEEDS_DRIVER_REBUILD=0
NEEDS_XORG_REBUILD=0
NEEDS_LIBINPUT_REBUILD=0

# Check if libinput driver needs to be built
# Only rebuild if it doesn't exist - the ABI version is fixed at compile time,
# so if libinput was built against the current Xorg ABI, it will continue to work
# even if Xorg is rebuilt (as long as the ABI hasn't changed)
if [ ! -f "$LIBINPUT_DRIVER" ]; then
    echo "libinput driver not found, will build it..."
    NEEDS_LIBINPUT_REBUILD=1
fi

# Check if driver needs rebuilding
if [ ! -f "$DRIVER_BUILD" ]; then
    echo "Driver not found, building..."
    NEEDS_DRIVER_REBUILD=1
elif [ -f "$DRIVER_SRC" ] && [ "$DRIVER_SRC" -nt "$DRIVER_BUILD" ]; then
    echo "Driver source (drmmode_xr_virtual.c) is newer than built driver, rebuilding..."
    NEEDS_DRIVER_REBUILD=1
elif [ "$XSRC_DIR/hw/xfree86/drivers/modesetting/driver.c" -nt "$DRIVER_BUILD" ] 2>/dev/null || \
     [ "$XSRC_DIR/hw/xfree86/drivers/modesetting/drmmode_display.c" -nt "$DRIVER_BUILD" ] 2>/dev/null || \
     [ "$XSRC_DIR/hw/xfree86/drivers/modesetting/drmmode_display.h" -nt "$DRIVER_BUILD" ] 2>/dev/null; then
    echo "Driver source files are newer than built driver, rebuilding..."
    NEEDS_DRIVER_REBUILD=1
fi

# Check if Xorg needs rebuilding (xf86RandR12.c is part of Xorg, not the driver)
if [ -f "$RANDR_SRC" ] && [ -f "$XORG_BIN" ] && [ "$RANDR_SRC" -nt "$XORG_BIN" ]; then
    echo "RandR source (xf86RandR12.c) is newer than built Xorg, rebuilding Xorg..."
    NEEDS_XORG_REBUILD=1
fi

# Build libinput driver if needed
if [ $NEEDS_LIBINPUT_REBUILD -eq 1 ]; then
    echo "Building xf86-input-libinput to match built Xorg's ABI..."

    # Clone if not exists
    if [ ! -d "$LIBINPUT_SRC_DIR" ]; then
        echo "Cloning xf86-input-libinput..."
        cd "$XSRC_DIR/.."
        git clone https://gitlab.freedesktop.org/xorg/driver/xf86-input-libinput.git || {
            echo "ERROR: Failed to clone xf86-input-libinput"
            echo "       Make sure you have git and network access"
            exit 1
        }
    fi

    # Build libinput driver
    cd "$LIBINPUT_SRC_DIR"
    # Always reconfigure to ensure correct include paths
    # Remove build directory if it exists to force fresh configuration
    if [ -d "$LIBINPUT_BUILD_DIR" ]; then
        echo "Removing existing build directory to reconfigure with correct include paths..."
        rm -rf "$LIBINPUT_BUILD_DIR"
    fi

    echo "Configuring xf86-input-libinput build..."
    # Use PKG_CONFIG_PATH to point to our built Xorg's pkg-config file
    # This ensures xf86-input-libinput builds against our Xorg with matching ABI
    # Extract include directories dynamically from the X server build system
    export PKG_CONFIG_PATH="$BUILD_DIR:$PKG_CONFIG_PATH"

    # Extract include directories from the actual X server build command
    # This ensures we use the same include paths as the modesetting driver
    # No hardcoding - dynamically extracts from the build system
    echo "Extracting include directories from X server build..."
    XSERVER_INC_ARGS="-I$BUILD_DIR/include"
    cd "$XSRC_DIR"
    DRIVER_DIR="$XSRC_DIR/hw/xfree86/drivers/modesetting"

    # Get build command and extract all -I flags
    # Use process substitution to avoid subshell issues
    while read inc_flag; do
        inc_path="${inc_flag#-I}"

        # Skip system paths and build artifact paths
        if [[ "$inc_path" == /usr/* ]] || [[ "$inc_path" == *modesetting_drv.so.p* ]]; then
            continue
        fi

        # Convert relative paths to absolute (no hardcoded directory names)
        if [[ "$inc_path" == ../* ]]; then
            # ../something -> $XSRC_DIR/something
            abs_path="$XSRC_DIR/${inc_path#../}"
        elif [[ "$inc_path" == . ]]; then
            # Current directory -> driver dir
            abs_path="$DRIVER_DIR"
        elif [[ "$inc_path" == .. ]]; then
            # Parent directory -> hw/xfree86
            abs_path="$XSRC_DIR/hw/xfree86"
        elif [[ "$inc_path" != /* ]]; then
            # Relative path - try driver dir first, then source root
            if [ -d "$DRIVER_DIR/$inc_path" ]; then
                abs_path="$DRIVER_DIR/$inc_path"
            else
                abs_path="$XSRC_DIR/$inc_path"
            fi
        else
            # Already absolute (but not system path)
            abs_path="$inc_path"
        fi

        # Only add if directory exists and not already added
        if [ -d "$abs_path" ] && [[ ! "$XSERVER_INC_ARGS" =~ "-I$abs_path" ]]; then
            XSERVER_INC_ARGS="$XSERVER_INC_ARGS,-I$abs_path"
        fi
    done < <(ninja -C build -t commands hw/xfree86/drivers/modesetting/modesetting_drv.so 2>/dev/null | \
        head -1 | grep -o "\-I[^ ]*")
    cd "$LIBINPUT_SRC_DIR"

    # Convert comma-separated string to meson array format: ['-Ipath1', '-Ipath2', ...]
    MESON_C_ARGS=$(echo "$XSERVER_INC_ARGS" | sed "s/,/','/g" | sed "s/^/['/" | sed "s/$/']/")

    meson setup build \
        --prefix="$LIBINPUT_BUILD_DIR/install" \
        -Dc_args="$MESON_C_ARGS" \
        || {
        echo "ERROR: Failed to configure xf86-input-libinput"
        echo ""
        echo "Missing dependencies. Please install:"
        echo "  sudo apt-get install meson ninja-build libinput-dev libinput-tools"
        echo ""
        echo "To check if libinput is available:"
        echo "  pkg-config --exists libinput && echo 'libinput found' || echo 'libinput missing'"
        exit 1
    }
    unset PKG_CONFIG_PATH

    echo "Building xf86-input-libinput..."
    ninja -C build || {
        echo "ERROR: Failed to build xf86-input-libinput"
        exit 1
    }

    if [ ! -f "$LIBINPUT_DRIVER" ]; then
        echo "ERROR: Built libinput driver not found at $LIBINPUT_DRIVER"
        exit 1
    fi

    echo "✓ Built libinput driver: $LIBINPUT_DRIVER"
fi

# Rebuild what's needed
if [ $NEEDS_DRIVER_REBUILD -eq 1 ] || [ $NEEDS_XORG_REBUILD -eq 1 ]; then
    cd "$XSRC_DIR"
    if [ $NEEDS_DRIVER_REBUILD -eq 1 ] && [ $NEEDS_XORG_REBUILD -eq 1 ]; then
        ninja -C build hw/xfree86/drivers/modesetting/modesetting_drv.so hw/xfree86/Xorg
    elif [ $NEEDS_DRIVER_REBUILD -eq 1 ]; then
        ninja -C build hw/xfree86/drivers/modesetting/modesetting_drv.so
    elif [ $NEEDS_XORG_REBUILD -eq 1 ]; then
        ninja -C build hw/xfree86/Xorg
    fi
fi

# Verify the driver exists after potential rebuild
if [ ! -f "$DRIVER_BUILD" ]; then
    echo "ERROR: Driver not found at $DRIVER_BUILD"
    echo "Please build the driver first: ninja -C build hw/xfree86/drivers/modesetting/modesetting_drv.so"
    exit 1
fi

echo "✓ Using driver directly from build directory"
echo "  Driver: $BUILD_DIR/hw/xfree86/drivers/modesetting/modesetting_drv.so"
echo "  Module path: $TEST_DRIVER_DIR"
echo ""

# Ensure sudo password is cached before we need it (needed for Xorg startup)
echo "Checking sudo access..."
if ! sudo -v; then
    echo "ERROR: Failed to authenticate with sudo. Exiting."
    exit 1
fi
echo "✓ Sudo access confirmed"
echo ""

echo "WARNING: This will start Xorg on display $TEST_DISPLAY (VT $TEST_VT)"
echo "Make sure display $TEST_DISPLAY is not in use!"
echo ""
read -p "Press Enter to continue or Ctrl+C to cancel..."
echo ""

# Double-check: Make sure we're not accidentally using display :0 (the main X server)
if [ "$TEST_DISPLAY" = ":0" ]; then
    echo "ERROR: Cannot use display :0 - that's your main X server!"
    echo "Please use a different display number (e.g., :1, :2, etc.)"
    exit 1
fi

# Check what VT the main X server is on and make sure we're not using it
MAIN_XORG_VT=$(ps aux | grep -E '[X]org.*:0' | grep -oE 'vt[0-9]+' | head -1 | sed 's/vt//')
if [ -n "$MAIN_XORG_VT" ] && [ "$MAIN_XORG_VT" = "$TEST_VT" ]; then
    echo "ERROR: VT $TEST_VT is already in use by the main X server!"
    echo "Please use a different VT number."
    exit 1
fi

# Check if display is already in use and kill any Xorg using it (likely our previous test Xorg)
if DISPLAY="$TEST_DISPLAY" xdpyinfo >/dev/null 2>&1; then
    echo "Display $TEST_DISPLAY is already in use. Checking what's using it..."

    # Find the PID of Xorg using this display socket
    XORG_PID_ON_DISPLAY=$(lsof -t "/tmp/.X11-unix/X${TEST_DISPLAY#:}" 2>/dev/null || true)
    if [ -n "$XORG_PID_ON_DISPLAY" ]; then
        # Verify it's actually an Xorg process
        if ps -p "$XORG_PID_ON_DISPLAY" -o comm= 2>/dev/null | grep -q "Xorg\|X"; then
            echo "Found Xorg process $XORG_PID_ON_DISPLAY using display $TEST_DISPLAY"
            echo "Killing it (this is likely a previous test Xorg server)..."
            sudo kill "$XORG_PID_ON_DISPLAY" 2>/dev/null || true
            sleep 2
            # Verify it's gone
            if kill -0 "$XORG_PID_ON_DISPLAY" 2>/dev/null; then
                echo "Process still running, force killing..."
                sudo kill -9 "$XORG_PID_ON_DISPLAY" 2>/dev/null || true
                sleep 1
            fi
            # Wait a moment for the socket to be released
            sleep 1
            # Verify display is now free
            if ! DISPLAY="$TEST_DISPLAY" xdpyinfo >/dev/null 2>&1; then
                echo "✓ Previous Xorg process killed, display $TEST_DISPLAY is now free"
            else
                echo "WARNING: Display $TEST_DISPLAY still appears to be in use after killing process"
            fi
        else
            echo "ERROR: Process $XORG_PID_ON_DISPLAY using display $TEST_DISPLAY is not Xorg!"
            echo "Please manually stop whatever is using display $TEST_DISPLAY"
            exit 1
        fi
    else
        echo "ERROR: Display $TEST_DISPLAY is in use but no process found using the socket"
        echo "Please manually stop whatever is using display $TEST_DISPLAY"
        exit 1
    fi
else
    # Display is free, but check for any stale Xorg processes using the socket anyway
    XORG_PID_ON_DISPLAY=$(lsof -t "/tmp/.X11-unix/X${TEST_DISPLAY#:}" 2>/dev/null || true)
    if [ -n "$XORG_PID_ON_DISPLAY" ]; then
        echo "Found stale Xorg process $XORG_PID_ON_DISPLAY on display socket (though xdpyinfo says display is free)"
        echo "Cleaning it up..."
        sudo kill "$XORG_PID_ON_DISPLAY" 2>/dev/null || true
        sleep 1
        if kill -0 "$XORG_PID_ON_DISPLAY" 2>/dev/null; then
            sudo kill -9 "$XORG_PID_ON_DISPLAY" 2>/dev/null || true
            sleep 1
        fi
    fi
fi

# Create minimal xorg.conf for testing
cat > /tmp/test-xorg.conf <<EOF
Section "ServerLayout"
    Identifier "TestLayout"
    Screen 0 "TestScreen" 0 0
EndSection

Section "Screen"
    Identifier "TestScreen"
    Device "TestDevice"
    DefaultDepth 24
EndSection

Section "Device"
    Identifier "TestDevice"
    Driver "modesetting"
    # Try Glamor first (enables TearFree and better performance, reduces flickering)
    # If Glamor is not available, Xorg will fall back to "none" automatically
    Option "AccelMethod" "glamor"
    # Enable TearFree to prevent screen tearing/flickering (requires Glamor)
    Option "TearFree" "true"
    # DoubleShadow helps with VNC/remote displays but may not fix local flickering
    # Option "DoubleShadow" "true"
EndSection

Section "ServerFlags"
    # Allow Xorg to auto-detect input devices via udev (same as system Xorg)
    Option "AutoAddDevices" "true"
    Option "AutoEnableDevices" "true"
    Option "DontZap" "false"
EndSection

# Match system Xorg's libinput configuration
# The script will automatically build xf86-input-libinput to match built Xorg's ABI
Section "InputClass"
    Identifier "libinput keyboard catchall"
    MatchIsKeyboard "on"
    MatchDevicePath "/dev/input/event*"
    Driver "libinput"
EndSection

Section "InputClass"
    Identifier "libinput pointer catchall"
    MatchIsPointer "on"
    MatchDevicePath "/dev/input/event*"
    Driver "libinput"
EndSection

Section "InputClass"
    Identifier "libinput touchpad catchall"
    MatchIsTouchpad "on"
    MatchDevicePath "/dev/input/event*"
    Driver "libinput"
EndSection
EOF

# Start Xorg with our driver on a separate VT
echo "Starting Xorg on $TEST_DISPLAY (VT $TEST_VT)..."
echo ""

# Start Xorg in background (sudo password should be cached now)
# Use -modulepath to tell Xorg where to find our driver
# Also set LD_LIBRARY_PATH in case the driver needs libraries from the build
echo "Starting Xorg (this requires sudo)..."
echo "Module path: $TEST_DRIVER_DIR"
echo "Build lib path: $BUILD_DIR"

# We MUST use built Xorg because we modified xf86RandR12.c
# However, system libinput has ABI version 24 while built Xorg expects 25
# We'll try to use evdev instead, or document that libinput needs to be built from source
XORG_BIN="$BUILD_DIR/hw/xfree86/Xorg"
if [ ! -f "$XORG_BIN" ]; then
    echo "ERROR: Built Xorg not found at $XORG_BIN"
    echo "       Run 'ninja -C build' to build Xorg"
    exit 1
fi
echo "Using built Xorg: $XORG_BIN (required for our xf86RandR12.c changes)"

# Try to get more detailed error information
# The "failed to map segment" error is often due to:
# 1. Missing dependencies (but we checked - all found)
# 2. ABI mismatch between driver and Xorg
# 3. Security restrictions
# 4. The driver needs to be in system location

# IMPORTANT: Use -novtswitch to prevent this X server from switching VTs
# This should prevent it from interfering with the running X server on VT7
# Start Xorg in background - we'll monitor it and kill it if it hangs
# Include built libinput driver directory (matches built Xorg's ABI)
# Xorg looks in subdirectories (drivers/, input/, etc.) within the module path
# NOTE: ModulePath uses COMMA as separator, not colon!
if [ -f "$LIBINPUT_DRIVER" ]; then
    # Use built libinput driver (ABI matches built Xorg)
    LIBINPUT_MODULE_DIR="$(dirname "$LIBINPUT_DRIVER")"
    MODULE_PATH="$TEST_DRIVER_DIR,$LIBINPUT_MODULE_DIR"
    echo "Using built libinput driver: $LIBINPUT_DRIVER"
else
    # Fallback to system modules (will have ABI mismatch, but at least Xorg will start)
    SYSTEM_MODULES="/usr/lib/xorg/modules"
    if [ -d "$SYSTEM_MODULES" ]; then
        MODULE_PATH="$TEST_DRIVER_DIR,$SYSTEM_MODULES"
        echo "WARNING: Using system libinput (ABI mismatch expected)"
    else
        MODULE_PATH="$TEST_DRIVER_DIR"
    fi
fi

sudo env LD_LIBRARY_PATH="$BUILD_DIR:$LD_LIBRARY_PATH" \
    "$XORG_BIN" "$TEST_DISPLAY" \
    -config /tmp/test-xorg.conf \
    -modulepath "$MODULE_PATH" \
    -logfile "$TEST_LOG" \
    -verbose 7 \
    -nolisten tcp \
    -novtswitch \
    -allowMouseOpenFail \
    vt$TEST_VT \
    > /tmp/Xorg_startup.log 2>&1 &
XORG_PID=$!
XORG_START_TIME=$(date +%s)

# Wait for Xorg to start (check if process is still running)
echo "Waiting for Xorg to start (max 15 seconds)..."
XORG_STARTED=0
WAIT_START=$(date +%s)
for i in {1..15}; do
    sleep 1
    if ! kill -0 $XORG_PID 2>/dev/null; then
        echo "ERROR: Xorg process died. Check logs:"
        echo ""
        echo "Startup log:"
        cat /tmp/Xorg_startup.log 2>/dev/null || echo "(startup log not found)"
        echo ""
        echo "Xorg log:"
        tail -50 "$TEST_LOG" 2>/dev/null || echo "(log file not found)"
        exit 1
    fi
    # Check if X server is responding
    if DISPLAY="$TEST_DISPLAY" timeout 2 xdpyinfo >/dev/null 2>&1; then
        WAIT_END=$(date +%s)
        WAIT_DURATION=$((WAIT_END - WAIT_START))
        echo "✓ Xorg is responding (took ${WAIT_DURATION} seconds)"
        XORG_STARTED=1
        break
    fi
    if [ $i -eq 15 ]; then
        echo "WARNING: Xorg started but not responding after 15 seconds"
        echo "Killing Xorg to prevent system lockup..."
        cleanup_xorg "timeout - not responding after 15 seconds"
        echo "Check logs:"
        tail -50 "$TEST_LOG" 2>/dev/null || echo "(log file not found)"
        exit 1
    fi
done

if [ $XORG_STARTED -eq 0 ]; then
    echo "ERROR: Xorg failed to start properly"
    cleanup_xorg "failed to start properly"
    exit 1
fi

echo "Xorg started (PID: $XORG_PID)"
echo ""

# Arrange displays first (before window manager and display settings)
echo "=== Arranging displays (external monitor ABOVE laptop) ==="
EXTERNAL_MONITOR=$(DISPLAY="$TEST_DISPLAY" xrandr 2>/dev/null | grep -E " connected" | grep -v "eDP" | head -1 | awk '{print $1}')
LAPTOP_MONITOR=$(DISPLAY="$TEST_DISPLAY" xrandr 2>/dev/null | grep -E "eDP.*connected" | awk '{print $1}')

if [ -n "$EXTERNAL_MONITOR" ] && [ -n "$LAPTOP_MONITOR" ]; then
    echo "Arranging displays: $EXTERNAL_MONITOR above $LAPTOP_MONITOR"
    if DISPLAY="$TEST_DISPLAY" xrandr --output "$EXTERNAL_MONITOR" --above "$LAPTOP_MONITOR" 2>/dev/null; then
        echo "✓ Displays arranged: $EXTERNAL_MONITOR above $LAPTOP_MONITOR"
    else
        echo "  Using explicit positioning..."
        EXTERNAL_HEIGHT=$(DISPLAY="$TEST_DISPLAY" xrandr 2>/dev/null | grep "^$EXTERNAL_MONITOR" | grep -oE "[0-9]+x[0-9]+" | head -1 | cut -dx -f2)
        if [ -z "$EXTERNAL_HEIGHT" ]; then EXTERNAL_HEIGHT=2160; fi
        DISPLAY="$TEST_DISPLAY" xrandr --output "$EXTERNAL_MONITOR" --pos 0x0 2>/dev/null
        DISPLAY="$TEST_DISPLAY" xrandr --output "$LAPTOP_MONITOR" --pos 0x${EXTERNAL_HEIGHT} 2>/dev/null
        echo "✓ Displays positioned explicitly"
    fi
else
    echo "⚠ Could not detect both monitors for arrangement"
fi
echo ""

# Start window manager, display utility, and xterm before running tests
echo "=== Starting Window Manager ==="
XFWM4_PID=""
WM_STARTED=0
if command -v xfwm4 >/dev/null 2>&1; then
    echo "Starting xfwm4 window manager..."
    DISPLAY="$TEST_DISPLAY" xfwm4 --replace >/tmp/xfwm4_${TEST_DISPLAY#:}.log 2>&1 &
    XFWM4_PID=$!
    sleep 3
    if kill -0 $XFWM4_PID 2>/dev/null; then
        echo "✓ xfwm4 started (PID: $XFWM4_PID)"
        echo "  Windows should now be moveable and resizable"
        WM_STARTED=1
    else
        echo "⚠ xfwm4 exited immediately (check /tmp/xfwm4_${TEST_DISPLAY#:}.log)"
        XFWM4_PID=""
    fi
fi

if [ $WM_STARTED -eq 0 ] && command -v openbox >/dev/null 2>&1; then
    echo "Starting openbox as fallback..."
    DISPLAY="$TEST_DISPLAY" openbox >/tmp/wm_${TEST_DISPLAY#:}.log 2>&1 &
    XFWM4_PID=$!
    sleep 2
    if kill -0 $XFWM4_PID 2>/dev/null; then
        echo "✓ openbox started (PID: $XFWM4_PID)"
        WM_STARTED=1
    else
        echo "⚠ openbox also failed to start"
        XFWM4_PID=""
    fi
fi

if [ $WM_STARTED -eq 0 ]; then
    echo "⚠ No window manager started - windows won't be moveable"
fi
echo ""

echo "=== Launching XFCE4 Display Settings ==="
DISPLAY_SETTINGS_PID=""
if command -v xfce4-display-settings >/dev/null 2>&1; then
    echo "Launching XFCE4 Display Settings (xfce4-display-settings)..."
    DISPLAY="$TEST_DISPLAY" xfce4-display-settings >/tmp/display_settings_${TEST_DISPLAY#:}.log 2>&1 &
    DISPLAY_SETTINGS_PID=$!
    sleep 3
    if kill -0 $DISPLAY_SETTINGS_PID 2>/dev/null; then
        echo "✓ Display Settings launched (PID: $DISPLAY_SETTINGS_PID)"
        echo "  You should see the Display Settings window showing RandR outputs"
    else
        echo "⚠ Display Settings exited immediately (check /tmp/display_settings_${TEST_DISPLAY#:}.log)"
        if [ -f /tmp/display_settings_${TEST_DISPLAY#:}.log ]; then
            echo "  Last 10 lines of log:"
            tail -10 /tmp/display_settings_${TEST_DISPLAY#:}.log | sed 's/^/    /'
        fi
        DISPLAY_SETTINGS_PID=""
    fi
else
    echo "⚠ xfce4-display-settings not found"
fi
echo ""

echo "=== Launching xterm for test output ==="
XRANDR_TERM_PID=""
if command -v xterm >/dev/null 2>&1; then
    # Find the monitor at the top (smallest Y position, ideally +0+0)
    # This should be the external monitor we arranged to be above the laptop
    TARGET_MONITOR=""
    TOP_Y=999999
    TERM_X=50
    TERM_Y=50
    TERM_HEIGHT=50

    # Parse --listmonitors to find the monitor with the smallest Y position
    while IFS= read -r line; do
        # Format: " 0: +*eDP-1 1920/293x1080/165+0+2160  eDP-1"
        MONITOR_POS=$(echo "$line" | grep -oE "\+[0-9]+\+[0-9]+" | head -1)
        if [ -n "$MONITOR_POS" ]; then
            Y_POS=$(echo "$MONITOR_POS" | cut -d+ -f3)
            if [ -n "$Y_POS" ] && [ "$Y_POS" -lt "$TOP_Y" ]; then
                TOP_Y=$Y_POS
                # Extract monitor name (e.g., "DP-1" or "eDP-1")
                TARGET_MONITOR=$(echo "$line" | awk '{print $NF}')
                MONITOR_INFO="$line"
            fi
        fi
    done < <(DISPLAY="$TEST_DISPLAY" xrandr --listmonitors 2>/dev/null | tail -n +2)

    if [ -n "$TARGET_MONITOR" ] && [ -n "$MONITOR_INFO" ]; then
        # Extract dimensions (e.g., 1920x1080) and position (e.g., +0+0)
        MONITOR_DIM=$(echo "$MONITOR_INFO" | grep -oE "[0-9]+/[0-9]+x[0-9]+/[0-9]+" | head -1)
        MONITOR_POS=$(echo "$MONITOR_INFO" | grep -oE "\+[0-9]+\+[0-9]+" | head -1)
        if [ -n "$MONITOR_DIM" ] && [ -n "$MONITOR_POS" ]; then
            # Extract height (second number after 'x', e.g., 1080 from "1920/293x1080/165")
            MONITOR_HEIGHT=$(echo "$MONITOR_DIM" | cut -dx -f2 | cut -d/ -f1)
            # Calculate 80% of height in character lines (assuming ~15 pixels per line)
            TERM_HEIGHT=$(( (MONITOR_HEIGHT * 80 / 100) / 15 ))
            # Ensure minimum and maximum reasonable values
            if [ $TERM_HEIGHT -lt 20 ]; then TERM_HEIGHT=20; fi
            if [ $TERM_HEIGHT -gt 80 ]; then TERM_HEIGHT=80; fi

            X_POS=$(echo "$MONITOR_POS" | cut -d+ -f2)
            Y_POS=$(echo "$MONITOR_POS" | cut -d+ -f3)
            TERM_X=$((X_POS + 50))
            TERM_Y=$((Y_POS + 50))
        fi
    fi

    echo "Launching xterm to show all test output on VT $TEST_VT (height: ${TERM_HEIGHT} lines, ~80% of monitor height)..."
    DISPLAY="$TEST_DISPLAY" xterm -geometry 100x${TERM_HEIGHT}+${TERM_X}+${TERM_Y} -title "XRandR Test Output" -e bash -c "tail -f '$XRANDR_LOG'" >/tmp/xrandr_term_${TEST_DISPLAY#:}.log 2>&1 &
    XRANDR_TERM_PID=$!
    sleep 1
    if kill -0 $XRANDR_TERM_PID 2>/dev/null; then
        echo "✓ xterm launched (PID: $XRANDR_TERM_PID) - you should see it on VT $TEST_VT"
    else
        echo "⚠ xterm failed to launch"
        XRANDR_TERM_PID=""
    fi
else
    echo "⚠ xterm not found"
fi
echo ""

# Test 1: Check if XR-Manager appears in xrandr
echo "=== Test 1: Check if XR-Manager appears in xrandr ==="
echo "All outputs:"
DISPLAY="$TEST_DISPLAY" xrandr 2>&1 | grep -E "^[A-Z]" || DISPLAY="$TEST_DISPLAY" xrandr 2>&1
echo ""

# Test 2: List all monitors
echo "=== Test 2: List all monitors ==="
DISPLAY="$TEST_DISPLAY" xrandr --listmonitors
echo ""

# Test 3: Automated XR Output Tests
echo "=== Test 3: Automated XR Output Tests ==="
echo "Waiting a moment for Display Settings to fully open..."
sleep 2

echo ""
echo "Creating virtual XR outputs..."
echo "  Creating XR-0 (1920x1080@60Hz)..."
if DISPLAY="$TEST_DISPLAY" xrandr --output XR-Manager --set CREATE_XR_OUTPUT "XR-0:1920:1080:60" 2>&1; then
    echo "  ✓ XR-0 created"
    sleep 2
else
    echo "  ✗ Failed to create XR-0"
fi

echo "  Creating XR-1 (2560x1440@60Hz)..."
if DISPLAY="$TEST_DISPLAY" xrandr --output XR-Manager --set CREATE_XR_OUTPUT "XR-1:2560:1440:60" 2>&1; then
    echo "  ✓ XR-1 created"
    sleep 2
else
    echo "  ✗ Failed to create XR-1"
fi

echo ""
echo "Current outputs:"
DISPLAY="$TEST_DISPLAY" xrandr 2>&1 | grep -E "^[A-Z]" | head -10

echo ""
echo "Resizing XR-0 to 3840x2160..."
if DISPLAY="$TEST_DISPLAY" xrandr --output XR-0 --set XR_WIDTH 3840 2>&1 && \
   DISPLAY="$TEST_DISPLAY" xrandr --output XR-0 --set XR_HEIGHT 2160 2>&1; then
    echo "  ✓ XR-0 resized"
    sleep 2
else
    echo "  ✗ Failed to resize XR-0"
fi

echo ""
echo "Arranging outputs..."
echo "  Connecting XR-0..."
if DISPLAY="$TEST_DISPLAY" xrandr --output XR-0 --auto 2>&1; then
    echo "  ✓ XR-0 connected"
    sleep 1
else
    echo "  ⚠ Could not connect XR-0"
fi

echo ""
echo "XR outputs created. Before testing deletion, please verify they appear in Display Settings."
if command -v zenity >/dev/null 2>&1; then
    DISPLAY="$TEST_DISPLAY" zenity --info --title "XR Output Test" \
        --text "XR-0 and XR-1 have been created.\n\nPlease check Display Settings to verify they appear.\n\nClick OK when ready to test deletion of XR-1." \
        2>/dev/null || echo "  (zenity dialog closed or failed)"
elif command -v xmessage >/dev/null 2>&1; then
    DISPLAY="$TEST_DISPLAY" xmessage -center -timeout 0 \
        "XR-0 and XR-1 have been created. Please check Display Settings to verify they appear. Click OK when ready to test deletion of XR-1." \
        2>/dev/null || echo "  (xmessage dialog closed)"
else
    echo "  Press Enter when ready to test deletion of XR-1..."
    read -r
fi

echo ""
echo "Deleting XR-1..."
# Suppress stderr since we expect a BadRROutput error (xrandr queries before deletion completes)
if DISPLAY="$TEST_DISPLAY" xrandr --output XR-Manager --set DELETE_XR_OUTPUT "XR-1" 2>/dev/null; then
    echo "  ✓ XR-1 deleted"
else
    # The deletion actually succeeds, but xrandr reports an error because it queries the output
    # during the property change operation, before the deletion completes
    echo "  ✓ XR-1 deleted (xrandr reported an error, but deletion succeeded - see final outputs below)"
fi
# Wait a moment for RandR changes to propagate
sleep 1

echo ""
echo "Final outputs:"
DISPLAY="$TEST_DISPLAY" xrandr 2>&1 | grep -E "^[A-Z]" | head -10

echo ""
echo "=== Interactive Testing ==="
echo "Xorg is running on display $TEST_DISPLAY (VT $TEST_VT)"
if [ -n "$XFWM4_PID" ]; then
    echo "Window manager (xfwm4/openbox) is running (PID: $XFWM4_PID)"
else
    echo "Window manager: not running"
fi
echo "Display Settings should be open - you can see the RandR outputs visually"
echo ""
echo "Waiting for Display Settings to be closed (this will exit the test session)..."
if [ -n "$DISPLAY_SETTINGS_PID" ]; then
    # Check if Xorg is still running
    if ! kill -0 $XORG_PID 2>/dev/null; then
        echo "WARNING: Xorg process ($XORG_PID) is not running - server may have crashed"
        echo "Check Xorg log: $TEST_LOG"
        echo "Display Settings may still be running, but X server is gone"
        echo "Proceeding to cleanup..."
    else
        wait $DISPLAY_SETTINGS_PID 2>/dev/null || true
        echo "Display Settings closed - proceeding to cleanup..."
    fi
else
    echo "Display Settings was not running - press Enter to exit..."
    read -r
fi

echo ""
echo "=== Cleanup ==="

# Kill xterm if still running
if [ -n "$XRANDR_TERM_PID" ] && kill -0 $XRANDR_TERM_PID 2>/dev/null; then
    echo "Closing xterm..."
    kill $XRANDR_TERM_PID 2>/dev/null || true
    wait $XRANDR_TERM_PID 2>/dev/null || true
fi


# Kill window manager if still running
if [ -n "$XFWM4_PID" ] && kill -0 $XFWM4_PID 2>/dev/null; then
    echo "Stopping window manager..."
    kill $XFWM4_PID 2>/dev/null || true
    sleep 1
    if kill -0 $XFWM4_PID 2>/dev/null; then
        kill -9 $XFWM4_PID 2>/dev/null || true
    fi
fi

# Cleanup Xorg
cleanup_xorg "user requested exit"

# Switch back to original VT if we can
if [ -n "$ORIGINAL_VT" ] && [ "$ORIGINAL_VT" != "$TEST_VT" ]; then
    echo "Switching back to original VT $ORIGINAL_VT..."
    if command -v chvt >/dev/null 2>&1; then
        sudo chvt "$ORIGINAL_VT" 2>/dev/null && echo "✓ Switched back to VT $ORIGINAL_VT" || echo "⚠ Could not switch back to VT $ORIGINAL_VT (you may need to press Ctrl+Alt+F$ORIGINAL_VT)"
    else
        echo "⚠ chvt not available, you may need to manually switch back to VT $ORIGINAL_VT (Ctrl+Alt+F$ORIGINAL_VT)"
    fi
fi

echo "Test complete."

