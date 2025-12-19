# Testing Virtual XR Connector

## Safe Testing Approaches

### Option 1: Use Test Script (Recommended)

A test script is provided that handles the setup:

```bash
cd /home/rebroad/src/xserver
./test_xr_virtual.sh
```

This script:
- Builds Xorg if needed
- Copies our patched driver to a test location
- Starts Xorg on display :1 (VT 8)
- Tests if XR-0 appears
- Shows log messages

**Note:** Make sure display :1 is not in use before running!

### Option 2: Test on Separate VT (More Realistic)

Run a test Xorg on a different virtual terminal:

```bash
# Switch to VT 8 (Ctrl+Alt+F8)
# Or run in a separate terminal session

# Copy our driver
sudo cp build/hw/xfree86/drivers/modesetting/modesetting_drv.so \
		/usr/lib/xorg/modules/drivers/modesetting_drv.so.test

# Create a test xorg.conf that uses our driver
# Run Xorg on display :1
sudo /home/rebroad/src/xserver/build/hw/xfree86/Xorg :1 -config /tmp/test-xorg.conf

# In another terminal, check outputs
DISPLAY=:1 xrandr --listmonitors
DISPLAY=:1 xrandr --listoutputs
```

### Option 3: Check Driver Loading (Safest - No X Server)

Just verify the driver loads and our function exists:

```bash
# Check if our function is exported
nm -D build/hw/xfree86/drivers/modesetting/modesetting_drv.so | grep drmmode_xr

# Check driver dependencies
ldd build/hw/xfree86/drivers/modesetting/modesetting_drv.so

# Verify initialization code path (static analysis)
grep -n "drmmode_xr_virtual_output_init" \
	 build/hw/xfree86/drivers/modesetting/modesetting_drv.so.p/*.o.d
```

### Option 4: Use Xephyr (Nested X Server)

Xephyr runs an X server in a window, good for visual testing:

```bash
# Build Xephyr
cd /home/rebroad/src/xserver
ninja -C build hw/kdrive/ephyr/Xephyr

# Run Xephyr (it will use our modesetting driver if configured)
build/hw/kdrive/ephyr/Xephyr :2 -screen 1920x1080 &

# Check outputs
DISPLAY=:2 xrandr --listmonitors
```

## What to Look For

1. **Xorg/Xvfb Log Messages:**
   ```
   Virtual XR connector (XR-0) created
   ```

2. **xrandr Output:**
   ```
   $ xrandr --listoutputs
   XR-0 connected (or disconnected)
   ```

3. **xrandr --listmonitors:**
   ```
   Monitors: 2
	 0: +*eDP-1 1920/344x1080/194mm
	 1: +XR-0 1920/0x1080/0mm
   ```

4. **Check AR_MODE Property:**
   ```
   $ xrandr --output XR-0 --get AR_MODE
   ```

## Expected Behavior (Current Implementation)

- **XR-0 should appear** in `xrandr --listoutputs` even if disabled
- **One default mode** (1920x1080) should be available
- **AR_MODE property** should exist (can query with `xrandr --output XR-0 --get AR_MODE`)
- **Output should be disabled by default** (won't show in `--listmonitors` until enabled)

## Troubleshooting

If XR-0 doesn't appear:

1. **Check Xorg logs:**
   ```bash
   grep -i "xr\|virtual\|drmmode_xr" /var/log/Xorg.0.log
   ```

2. **Verify driver loaded:**
   ```bash
   grep "modesetting" /var/log/Xorg.0.log
   ```

3. **Check for errors:**
   ```bash
   grep -i "error\|fail" /var/log/Xorg.0.log | tail -20
   ```

4. **Verify initialization was called:**
   ```bash
   # Our code should log: "Virtual XR connector (XR-0) created"
   grep "XR-0" /var/log/Xorg.0.log
   ```

## Next Steps After Testing

Once we verify XR-0 appears:
1. Test enabling it: `xrandr --output XR-0 --auto`
2. Test setting a mode: `xrandr --output XR-0 --mode 1920x1080`
3. Test AR_MODE property: `xrandr --output XR-0 --set AR_MODE 1`
4. Verify it appears in XFCE display tool

