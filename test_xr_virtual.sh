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
        sudo kill "$XORG_PID" 2>/dev/null || true
        sleep 0.5
        if kill -0 "$XORG_PID" 2>/dev/null; then
            if [ $quick_exit -eq 0 ]; then
                echo "Force killing Xorg (PID: $XORG_PID)..."
            fi
            sudo kill -9 "$XORG_PID" 2>/dev/null || true
            force_killed=1
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

# Set up trap to ensure cleanup happens on exit
trap 'cleanup_xorg "script exit (trap)"' EXIT
trap 'cleanup_xorg "interrupted (SIGINT)"' INT
trap 'cleanup_xorg "terminated (SIGTERM)"' TERM

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

# Check if driver or related files need to be rebuilt (source is newer than built driver)
DRIVER_SRC="$XSRC_DIR/hw/xfree86/drivers/modesetting/drmmode_xr_virtual.c"
DRIVER_BUILD="$BUILD_DIR/hw/xfree86/drivers/modesetting/modesetting_drv.so"
RANDR_SRC="$XSRC_DIR/hw/xfree86/modes/xf86RandR12.c"
XORG_BIN="$BUILD_DIR/hw/xfree86/Xorg"

NEEDS_DRIVER_REBUILD=0
NEEDS_XORG_REBUILD=0

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

# Check if display is already in use
if DISPLAY="$TEST_DISPLAY" xdpyinfo >/dev/null 2>&1; then
    echo "ERROR: Display $TEST_DISPLAY is already in use!"
    echo "Please stop the X server on $TEST_DISPLAY first, or use a different display."
    exit 1
fi

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

# Only kill Xorg processes using this specific display (if any)
# Find the PID of Xorg using this display
XORG_PID_ON_DISPLAY=$(lsof -t "/tmp/.X11-unix/X${TEST_DISPLAY#:}" 2>/dev/null || true)
if [ -n "$XORG_PID_ON_DISPLAY" ]; then
    echo "Found Xorg process $XORG_PID_ON_DISPLAY using display $TEST_DISPLAY"
    echo "Killing only that process..."
    sudo kill "$XORG_PID_ON_DISPLAY" 2>/dev/null || true
    sleep 1
    # Verify it's gone
    if kill -0 "$XORG_PID_ON_DISPLAY" 2>/dev/null; then
        echo "WARNING: Process still running, using kill -9..."
        sudo kill -9 "$XORG_PID_ON_DISPLAY" 2>/dev/null || true
        sleep 1
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
    Option "AccelMethod" "none"
EndSection

Section "ServerFlags"
    # Allow Xorg to auto-detect input devices via udev/libinput
    # Note: Physical input devices may not be available if already grabbed by
    # the main X server on :0. The test will still work using xdotool (XTEST).
    Option "AutoAddDevices" "true"
    Option "AutoEnableDevices" "true"
    # Don't fail if input devices can't be opened (they may be grabbed by :0)
    Option "DontZap" "false"
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

# Try using system Xorg first (more compatible), fall back to built Xorg
XORG_BIN="/usr/bin/Xorg"
if [ ! -f "$XORG_BIN" ] || ! "$XORG_BIN" -version >/dev/null 2>&1; then
    XORG_BIN="$BUILD_DIR/hw/xfree86/Xorg"
    echo "Using built Xorg: $XORG_BIN"
else
    echo "Using system Xorg: $XORG_BIN"
fi

# Try to get more detailed error information
# The "failed to map segment" error is often due to:
# 1. Missing dependencies (but we checked - all found)
# 2. ABI mismatch between driver and Xorg
# 3. Security restrictions
# 4. The driver needs to be in system location

# IMPORTANT: Use -novtswitch to prevent this X server from switching VTs
# This should prevent it from interfering with the running X server on VT7
# Start Xorg in background - we'll monitor it and kill it if it hangs
sudo env LD_LIBRARY_PATH="$BUILD_DIR:$LD_LIBRARY_PATH" \
    "$XORG_BIN" "$TEST_DISPLAY" \
    -config /tmp/test-xorg.conf \
    -modulepath "$TEST_DRIVER_DIR" \
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
        export WAIT_DURATION WAIT_START  # Export for use in XLogo section
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

# Ensure WAIT_DURATION is set (in case loop didn't set it)
if [ -z "$WAIT_DURATION" ]; then
    WAIT_END=$(date +%s)
    WAIT_DURATION=$((WAIT_END - WAIT_START))
fi

echo "Xorg started (PID: $XORG_PID)"
echo ""

# Start XLogo immediately for visual feedback (before running other tests)
XLOGO_PIDS=""
WM_PID=""
if command -v xlogo >/dev/null 2>&1; then
    XLOGO_START_TIME=$(date +%s)
    echo "Starting XLogo immediately for visual feedback..."
    # Try to get outputs immediately - if xrandr isn't ready yet, wait briefly and retry
    # Extract output positions from xrandr output
    XRANDR_AWK_SCRIPT='{
        output=$1
        match($0, /\+[0-9]+\+[0-9]+/)
        if (RSTART > 0) {
            pos=substr($0, RSTART+1, RLENGTH-1)
            split(pos, coords, "+")
            x=coords[1]
            y=coords[2]
            print output " " x " " y
        } else {
            print output " 0 0"
        }
    }'

    XRANDR_START=$(date +%s)
    OUTPUT_INFO=$(DISPLAY="$TEST_DISPLAY" timeout 1 xrandr 2>/dev/null | grep -E " connected" | awk "$XRANDR_AWK_SCRIPT")
    XRANDR_END=$(date +%s)
    XRANDR_DURATION=$((XRANDR_END - XRANDR_START))

    # If no outputs found, wait briefly for RandR to finish probing and retry
    if [ -z "$OUTPUT_INFO" ]; then
        sleep 0.5  # Only wait if outputs aren't ready yet
        OUTPUT_INFO=$(DISPLAY="$TEST_DISPLAY" timeout 1 xrandr 2>/dev/null | grep -E " connected" | awk "$XRANDR_AWK_SCRIPT")
    fi

    # Fallback: try to get any outputs without position info
    if [ -z "$OUTPUT_INFO" ]; then
        OUTPUTS=$(DISPLAY="$TEST_DISPLAY" timeout 1 xrandr 2>/dev/null | grep -E "^[A-Z]" | awk '{print $1}' | head -3)
        if [ -n "$OUTPUTS" ]; then
            OUTPUT_INFO=""
            for output in $OUTPUTS; do
                OUTPUT_INFO="$OUTPUT_INFO $output 0 0"
            done
        fi
    fi

    if [ -n "$OUTPUT_INFO" ]; then
        echo "$OUTPUT_INFO" | while read output x_pos y_pos; do
            win_x=$((x_pos + 100))
            win_y=$((y_pos + 100))
            DISPLAY="$TEST_DISPLAY" xlogo -geometry 200x200+${win_x}+${win_y} >/tmp/xlogo_${output}.log 2>&1 &
            echo $! >> /tmp/xlogo_pids.txt
        done

        if [ -f /tmp/xlogo_pids.txt ]; then
            XLOGO_PIDS=$(cat /tmp/xlogo_pids.txt | tr '\n' ' ')
            rm -f /tmp/xlogo_pids.txt
        fi
        XLOGO_END_TIME=$(date +%s)
        XLOGO_TOTAL_DURATION=$((XLOGO_END_TIME - XLOGO_START_TIME))
        echo "✓ XLogo started (PIDs:$XLOGO_PIDS) - windows should be visible now"
        echo "  Timing: Xorg startup ${WAIT_DURATION}s, xrandr query ${XRANDR_DURATION}s, XLogo launch ${XLOGO_TOTAL_DURATION}s total"
    fi
fi
echo ""

# Test 1: Check if XR-0 appears in xrandr
echo "=== Test 1: Check if XR-0 appears in xrandr ==="
echo "All outputs:"
DISPLAY="$TEST_DISPLAY" xrandr 2>&1 | grep -E "^[A-Z]" || DISPLAY="$TEST_DISPLAY" xrandr 2>&1
echo ""
echo "Note: 'Screen 0' shows the combined screen space (all outputs together)."
echo "      Your setup: eDP-1 (1920x1080) + DP-1 (1920x1080) = 3840x1080 total"
echo ""
if DISPLAY="$TEST_DISPLAY" xrandr 2>&1 | grep -i "XR-0" >/dev/null; then
    echo "✓ XR-0 found!"
else
    echo "✗ XR-0 not found"
    echo "Checking Xorg log for XR-0 initialization..."
    grep -i "XR-0\|Virtual XR\|post_screen" "$TEST_LOG" 2>/dev/null | tail -10 || echo "No XR-0 messages in log"
fi

echo ""
echo "=== Test 2: List all monitors ==="
DISPLAY="$TEST_DISPLAY" xrandr --listmonitors

echo ""
echo "=== Test 3: Get XR-0 info ==="
DISPLAY="$TEST_DISPLAY" xrandr --output XR-0 --query 2>&1 || echo "XR-0 query failed (might be expected if not fully implemented)"

echo ""
echo "=== Test 4: Check AR_MODE property ==="
echo "Checking Xorg log for AR_MODE property creation..."
AR_MODE_LOGS=$(grep -i "AR_MODE" "$TEST_LOG" 2>/dev/null | tail -5)
if [ -n "$AR_MODE_LOGS" ]; then
    echo "AR_MODE property creation attempts in log:"
    echo "$AR_MODE_LOGS"
fi

if DISPLAY="$TEST_DISPLAY" xrandr --output XR-0 --prop 2>&1 | grep -i "AR_MODE" >/dev/null; then
    echo "✓ AR_MODE property found in xrandr:"
    DISPLAY="$TEST_DISPLAY" xrandr --output XR-0 --prop 2>&1 | grep -i "AR_MODE" -A2
else
    echo "✗ AR_MODE property not found in xrandr"
    echo "All properties for XR-0:"
    DISPLAY="$TEST_DISPLAY" xrandr --output XR-0 --prop 2>&1 | head -20 || echo "Could not query XR-0 properties (output may not exist)"
    echo ""
    echo "Note: Property may have been created but output was recreated, losing the property."
    echo "Check the Xorg log above for AR_MODE creation messages."
fi

echo ""
echo "=== Test 5: Check Xorg log for initialization ==="
if grep -i "virtual.*xr\|xr-0.*created\|post_screen" "$TEST_LOG" 2>/dev/null; then
    echo "✓ Found initialization messages in log:"
    grep -i "virtual.*xr\|xr-0\|post_screen" "$TEST_LOG" | tail -10
else
    echo "✗ No initialization message found"
    echo "Last 30 lines of log:"
    tail -30 "$TEST_LOG"
fi

echo ""
echo "=== Test 6: Start window manager (for cursor visibility) ==="
# Try to find a lightweight window manager
WM_CMD=""
if command -v openbox >/dev/null 2>&1; then
    WM_CMD="openbox"
elif command -v twm >/dev/null 2>&1; then
    WM_CMD="twm"
elif command -v metacity >/dev/null 2>&1; then
    WM_CMD="metacity --replace"
elif command -v xfwm4 >/dev/null 2>&1; then
    WM_CMD="xfwm4 --replace"
fi

if [ -n "$WM_CMD" ]; then
    echo "Starting window manager: $WM_CMD"
    DISPLAY="$TEST_DISPLAY" $WM_CMD >/tmp/wm_${TEST_DISPLAY#:}.log 2>&1 &
    WM_PID=$!
    sleep 2  # Give WM time to start
    if kill -0 $WM_PID 2>/dev/null; then
        echo "✓ Window manager started (PID: $WM_PID)"
    else
        echo "⚠ Window manager may have exited (check /tmp/wm_${TEST_DISPLAY#:}.log)"
        WM_PID=""
    fi
else
    echo "⚠ No window manager found (tried: openbox, twm, metacity, xfwm4)"
    echo "  Cursor may not be visible without a window manager"
fi

echo ""
echo "=== Test 7: Test mouse pointer (with window manager) ==="
if DISPLAY="$TEST_DISPLAY" xsetroot -cursor_name left_ptr >/dev/null 2>&1; then
    echo "✓ Mouse pointer cursor set"

    # Try to actually move the pointer to verify it's working
    if command -v xdotool >/dev/null 2>&1; then
        echo "  Testing pointer movement with xdotool..."
        # Get current position
        OLD_POS=$(DISPLAY="$TEST_DISPLAY" xdotool getmouselocation 2>/dev/null | awk '{print $1" "$2}' | sed 's/[xy]://g')
        if [ -n "$OLD_POS" ]; then
            echo "  Current mouse position: $OLD_POS"
            # Move mouse to a visible position (center of first screen)
            DISPLAY="$TEST_DISPLAY" xdotool mousemove 960 540 >/dev/null 2>&1
            sleep 0.3
            # Move mouse to verify it moves
            DISPLAY="$TEST_DISPLAY" xdotool mousemove 1000 600 >/dev/null 2>&1
            sleep 0.3
            NEW_POS=$(DISPLAY="$TEST_DISPLAY" xdotool getmouselocation 2>/dev/null | awk '{print $1" "$2}' | sed 's/[xy]://g')
            if [ -n "$NEW_POS" ]; then
                echo "  New mouse position: $NEW_POS"
                if [ "$OLD_POS" != "$NEW_POS" ]; then
                    echo "  ✓ Mouse pointer is moveable (moved from $OLD_POS to $NEW_POS)"
                else
                    echo "  ⚠ Mouse pointer position didn't change"
                fi
            else
                echo "  ⚠ Could not get new mouse position"
            fi
        else
            echo "  ⚠ Could not get mouse position"
        fi
    fi

    # Check for pointer devices
    if command -v xinput >/dev/null 2>&1; then
        echo "  Checking pointer devices..."
        POINTER_DEVICES=$(DISPLAY="$TEST_DISPLAY" xinput list 2>/dev/null | grep -i "pointer\|mouse\|slave" | head -5)
        if [ -n "$POINTER_DEVICES" ]; then
            echo "  Found pointer devices:"
            echo "$POINTER_DEVICES" | sed 's/^/    /'
        fi
    fi

    if [ -n "$WM_PID" ]; then
        echo "  ✓ Window manager is running - cursor should be visible"
        echo ""
        echo "  ⚠ PHYSICAL MOUSE/TOUCHPAD WON'T WORK:"
        echo "     The main X server on :0 has already grabbed your input devices."
        echo "     This is EXPECTED and SAFE - the test X server won't interfere with"
        echo "     your main session. Only xdotool (XTEST) mouse movement works."
        echo "     To test with physical mouse, you'd need to stop the main X server first."
    else
        echo "  ⚠ No window manager - cursor may not be visible"
    fi
else
    echo "✗ Failed to set mouse pointer"
fi

echo ""
echo "=== Test 8: Verify X11 graphics capabilities ==="
if DISPLAY="$TEST_DISPLAY" xdpyinfo >/dev/null 2>&1; then
    echo "✓ X server is responding to X11 protocol"
else
    echo "✗ X server not responding"
fi

# Keep XLogo running for a bit longer so user can see it
if [ -n "$XLOGO_PIDS" ]; then
    echo ""
    echo "XLogo windows will remain visible for 5 more seconds..."
    sleep 5

    # Kill all xlogo processes
    for pid in $XLOGO_PIDS; do
        if kill -0 $pid 2>/dev/null; then
            kill $pid 2>/dev/null || true
            wait $pid 2>/dev/null || true
        fi
    done
    echo "✓ XLogo windows closed"
fi

echo ""
echo "=== Cleanup ==="
# Kill window manager if still running
if [ -n "$WM_PID" ] && kill -0 $WM_PID 2>/dev/null; then
    echo "Stopping window manager..."
    kill $WM_PID 2>/dev/null || true
    wait $WM_PID 2>/dev/null || true
fi

# Cleanup is handled by the trap, but we'll do it explicitly here too
# Temporarily disable the trap to avoid double cleanup
trap - EXIT INT TERM
cleanup_xorg "normal test completion"

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

