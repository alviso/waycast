/**
 * ui.h — top-level UI: screen S1 (map + status bar + report FAB),
 * S2 (two-tap report flow), S3 (hazard detail). Target-agnostic; the
 * target's main() provides the display and calls ui_init() once.
 */
#pragma once

void ui_init(void);
