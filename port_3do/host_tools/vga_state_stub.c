/*
 * vga_state_stub.c — host-side replacement for openomf-master/src/video/vga_state.c.
 *
 * The original vga_state.c transitively includes <SDL.h> via game_state.h. For
 * a host-side asset converter we never call the vga_state palette mutation
 * functions, but a few symbols are still referenced from palette.c's
 * setter/mutator paths. With -ffunction-sections + --gc-sections the linker
 * drops the unreachable code, but it still wants the symbols declared.
 *
 * Every function here is a no-op. If the dump_har call path ever does invoke
 * one of these, we'll notice — sd_bk_load and sd_sprite_rgba_decode have been
 * verified to NOT touch vga_state in the load direction.
 */

#include "video/vga_state.h"

#include <stdbool.h>
#include <stddef.h>

void vga_state_init(void) {}
void vga_state_close(void) {}
void vga_state_render(void) {}
void vga_state_mark_palette_flushed(void) {}
void vga_state_mark_remaps_flushed(void) {}
void vga_state_mark_dirty(void) {}

bool vga_state_is_palette_dirty(vga_palette **palette, vga_index *dirty_range_first, vga_index *dirty_range_last) {
    (void)palette; (void)dirty_range_first; (void)dirty_range_last;
    return false;
}

bool vga_state_is_remap_dirty(vga_remap_tables **remaps) {
    (void)remaps;
    return false;
}

void vga_state_push_palette(void) {}
void vga_state_pop_palette(void) {}
void vga_state_mul_base_palette(vga_index start, vga_index end, float multiplier) {
    (void)start; (void)end; (void)multiplier;
}
void vga_state_set_remaps_from(const vga_remap_tables *src) { (void)src; }
void vga_state_set_base_palette_from(const vga_palette *src) { (void)src; }
void vga_state_set_base_palette_from_range(const vga_palette *src, vga_index dst_start,
                                           vga_index src_start, vga_index count) {
    (void)src; (void)dst_start; (void)src_start; (void)count;
}
void vga_state_set_base_palette_index(vga_index index, const vga_color *color) {
    (void)index; (void)color;
}
void vga_state_set_base_palette_range(vga_index start, vga_index count, vga_color *src_colors) {
    (void)start; (void)count; (void)src_colors;
}
void vga_state_copy_base_palette_range(vga_index dst, vga_index src, vga_index count) {
    (void)dst; (void)src; (void)count;
}
void vga_state_enable_palette_transform(vga_palette_transform transform_callback, void *userdata) {
    (void)transform_callback; (void)userdata;
}
void vga_state_debug_screenshot(const path *filename) { (void)filename; }
