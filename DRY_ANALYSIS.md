# DRY Analysis: Code Duplication in GBM Format Selection

## Duplicated Code

The GBM format selection logic based on screen depth is duplicated in two places:

1. **`drmmode_create_bo()`** in `drmmode_display.c` (lines 1120-1133)
2. **`drmmode_xr_create_offscreen_framebuffer()`** in `drmmode_xr_virtual.c` (lines 1029-1042)

### Current Duplication:

```c
/* Identical switch statement in both functions */
switch (pScrn->depth) {  /* or drmmode->scrn->depth */
case 15:
    format = GBM_FORMAT_ARGB1555;
    break;
case 16:
    format = GBM_FORMAT_RGB565;
    break;
case 30:
    format = GBM_FORMAT_ARGB2101010;
    break;
default:
    format = GBM_FORMAT_ARGB8888;
    break;
}
```

## Proposed Solution

Extract the format selection into a helper function. Since `drmmode_create_bo()` is static, we have a few options:

### Option 1: Add to `drmmode_display.h` as a static inline function (Recommended)

**Pros:**
- Simple and efficient (inline, no function call overhead)
- Accessible from both files
- Header file is the right place for utility functions
- No changes needed to existing static function signatures

**Cons:**
- Requires including `gbm.h` in the header (but we already do conditionally)

**Implementation:**
```c
/* In drmmode_display.h, inside #ifdef GLAMOR_HAS_GBM block */
static inline uint32_t
drmmode_get_gbm_format_for_depth(int depth)
{
    switch (depth) {
    case 15:
        return GBM_FORMAT_ARGB1555;
    case 16:
        return GBM_FORMAT_RGB565;
    case 30:
        return GBM_FORMAT_ARGB2101010;
    default:
        return GBM_FORMAT_ARGB8888;
    }
}
```

**Usage in both files:**
```c
uint32_t format = drmmode_get_gbm_format_for_depth(pScrn->depth);
```

### Option 2: Make a non-static function in `drmmode_display.c`

**Pros:**
- Keeps GBM includes out of header if needed
- Standard function, not inline

**Cons:**
- Requires changing function linkage (making it non-static)
- Less efficient (function call overhead, though minimal)
- Would need to add declaration to header anyway

### Option 3: Keep duplication (Current approach)

**Pros:**
- No changes needed
- Each function is self-contained

**Cons:**
- Code duplication
- If format mapping changes, need to update multiple places
- Violates DRY principle

## Recommendation

**Use Option 1 (static inline function in header)** because:
1. It's the simplest solution
2. Eliminates duplication completely
3. Inline functions have zero overhead
4. The format mapping logic is unlikely to change, but if it does, we only need to update one place
5. The header file already conditionally includes GBM headers when needed

The code change would be minimal:
- Add the helper function to `drmmode_display.h`
- Replace the switch statements in both `drmmode_create_bo()` and `drmmode_xr_create_offscreen_framebuffer()` with a single function call

