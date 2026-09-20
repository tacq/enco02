#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register a streaming decoder for 16-colour (I4) C-array images.
 *
 * Must be called after lv_init() and before any I4 image is drawn.  Safe to call more than once.
 */
void enco_i4_decoder_init(void);

#ifdef __cplusplus
}
#endif
