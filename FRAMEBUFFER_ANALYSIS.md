# On-Screen vs Off-Screen Framebuffer Implementation Analysis

## How On-Screen Framebuffers Work

### Initial Setup (Screen-wide)
1. **`drmmode_create_initial_bos()`** (line 4437 in drmmode_display.c):
   - Creates the main `drmmode->front_bo` buffer object for the entire screen
   - Uses `drmmode_create_bo()` which automatically chooses:
     - GBM buffer objects (if glamor/GBM available) for GPU-optimized rendering
     - Dumb buffers (fallback) for CPU-accessible memory
   - Stores BO in `drmmode->front_bo`

2. **BO Import as Framebuffer**:
   - `drmmode_bo_import()` is called to create a DRM framebuffer from the BO
   - Gets an FB ID stored in `drmmode->fb_id`
   - This FB ID is used for actual hardware scanout

3. **Root Pixmap Creation** (CreateScreenResources):
   - Maps the BO memory: `drmmode_map_front_bo()` → `drmmode_bo_map()`
   - Gets the mapped pointer
   - Calls `ModifyPixmapHeader()` on the root pixmap to back it with the mapped memory
   - The root pixmap now points directly to the framebuffer memory

### Per-CRTC Scanout Pixmaps (For Specific Outputs)
When a CRTC needs to scanout a specific pixmap (e.g., for multi-head or shared pixmaps):

1. **`drmmode_set_target_scanout_pixmap_cpu()`** (line 2014):
   - Expects a pixmap that already has a BO set up in its private data
   - Maps the BO: `drmmode_map_secondary_bo()` → uses pixmap's `backing_bo`
   - Sets `ppix->devPrivate.ptr = ptr` to point pixmap at mapped memory
   - Creates damage tracking: `DamageCreate()` and `DamageRegister()`
   - Creates DRM framebuffer if needed: `drmModeAddFB()` using `ppriv->backing_bo->handle`
   - Stores FB ID in `ppriv->fb_id`

2. **`drmmode_shadow_fb_create()`** (line 2168) - Used for rotated/shadow pixmaps:
   - Takes a BO and creates a pixmap from it
   - Maps the BO: `drmmode_bo_map()`
   - Creates pixmap header: `drmmode_create_pixmap_header()` with mapped memory
   - Links BO to pixmap: `drmmode_set_pixmap_bo()` (stores BO in pixmap private data)

### Key Functions Used:
- `drmmode_create_bo()` - Creates BO (GBM or dumb, automatic selection)
- `drmmode_bo_import()` - Creates DRM framebuffer from BO, returns FB ID
- `drmmode_bo_map()` - Maps BO to CPU-accessible memory, returns pointer
- `drmmode_create_pixmap_header()` - Creates pixmap with specific memory backing
- `drmmode_set_pixmap_bo()` - Links BO to pixmap private data
- `drmmode_map_secondary_bo()` - Maps a pixmap's backing BO

## Our Off-Screen Implementation

### Current Approach:
1. **Manual BO Creation**:
   - Directly calls `dumb_bo_create()` (bypasses `drmmode_create_bo()`)
   - Only uses dumb buffers (doesn't support GBM/GPU-optimized paths)

2. **FB Import**:
   - Uses `drmmode_bo_import()` ✓ (reuses existing function)
   - Stores FB ID in `vout->framebuffer_id`

3. **Mapping**:
   - Directly calls `dumb_bo_map()` (bypasses `drmmode_bo_map()`)

4. **Pixmap Creation**:
   - Manually creates pixmap with `CreatePixmap()`
   - Uses `ModifyPixmapHeader()` to back it with mapped memory
   - Manually sets up pixmap private data: `ppriv->fb_id` and `ppriv->backing_bo`
   - Does NOT use `drmmode_set_pixmap_bo()` (which is static anyway)

## Comparison: Are We Reusing Code?

### What We're Reusing:
✅ `drmmode_bo_import()` - For creating DRM framebuffers from BOs
✅ `drmmode_bo_destroy()` - For cleanup
✅ `drmmode_bo_get_pitch()` / `drmmode_bo_get_handle()` - For BO metadata
✅ Basic BO creation pattern (but using `dumb_bo_create` directly)

### What We're NOT Reusing:
❌ `drmmode_create_bo()` - It's static, and we're using `dumb_bo_create` directly
❌ `drmmode_bo_map()` - It's likely static, we're using `dumb_bo_map` directly  
❌ `drmmode_shadow_fb_create()` - It's static and CRTC-specific
❌ `drmmode_set_pixmap_bo()` - It's static
❌ Damage tracking setup (not needed for our use case)

## Should We Adapt Existing Code?

### Option 1: Current Approach (Manual Implementation)
**Pros:**
- ✅ Simple and straightforward
- ✅ Clear control over each step
- ✅ Doesn't require exposing internal functions
- ✅ Works for our specific use case

**Cons:**
- ❌ Doesn't support GBM/GPU-optimized paths (only dumb buffers)
- ❌ Code duplication (similar patterns exist in existing code)
- ❌ If BO creation logic changes, we'd need to update our code separately

### Option 2: Make Internal Functions Non-Static
**Pros:**
- ✅ Could reuse `drmmode_create_bo()` for GBM support
- ✅ Could reuse `drmmode_bo_map()` wrapper
- ✅ Could reuse `drmmode_shadow_fb_create()` pattern
- ✅ Single code path for BO/pixmap creation

**Cons:**
- ❌ Would need to expose internal implementation details
- ❌ Might break encapsulation
- ❌ Existing code may depend on these being static

### Option 3: Extract Common Code into New Helper Functions
**Pros:**
- ✅ Could create `drmmode_create_pixmap_from_bo()` helper
- ✅ Maintains encapsulation
- ✅ Both on-screen and off-screen code could use it
- ✅ Clean separation of concerns

**Cons:**
- ❌ More refactoring required
- ❌ Need to ensure no regressions in existing code

## Assessment: Is Our Current Approach Best?

### For Initial Implementation: **YES** ✅

Our current approach is reasonable for several reasons:

1. **Simplicity**: We're building a minimal implementation that works. The code is clear and easy to understand.

2. **Independence**: We're not tightly coupled to internal implementation details that might change.

3. **Functionality**: We get all the functionality we need (BO creation, FB import, pixmap creation).

4. **Static Functions**: Most helper functions we'd want to reuse are static, meaning they're intentionally internal. Bypassing them with direct calls (`dumb_bo_create`, `dumb_bo_map`) is acceptable.

### Potential Future Improvements:

1. **GBM Support**: If we want GPU-optimized rendering later, we could:
   - Check if `drmmode_create_bo()` could be made non-static (if maintainers approve)
   - Or replicate its GBM logic in our function

2. **Code Sharing**: If maintainers want to reduce duplication, we could:
   - Propose extracting a `drmmode_create_pixmap_from_bo()` helper
   - Both existing shadow code and our code could use it

3. **Damage Tracking**: If we need efficient updates (probably not for our use case, as renderer polls):
   - Could add damage tracking similar to `drmmode_set_target_scanout_pixmap_cpu()`
   - Probably not needed since renderer will poll/capture frames directly

## Conclusion

Our approach is **appropriate for now**. We're:
- ✅ Reusing what we can (FB import, destruction helpers)
- ✅ Using the same low-level APIs directly (dumb buffers)
- ✅ Not breaking encapsulation unnecessarily
- ✅ Keeping code simple and maintainable

The main trade-off is missing GBM/GPU optimization paths, but for initial implementation with dumb buffers (CPU-accessible), this is acceptable. We can always enhance it later if needed.

