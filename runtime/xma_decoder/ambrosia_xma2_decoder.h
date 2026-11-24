#ifndef AMBROSIA_XMA2_DECODER_H
#define AMBROSIA_XMA2_DECODER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AMBROSIA_XMA2_PACKET_BYTES 2048u

typedef struct AmbrosiaXma2Decoder AmbrosiaXma2Decoder;
typedef struct AmbrosiaXma2PcmBlock AmbrosiaXma2PcmBlock;

enum AmbrosiaXma2Status {
  AMBROSIA_XMA2_OK = 0,
  AMBROSIA_XMA2_INVALID_ARGUMENT = 1,
  AMBROSIA_XMA2_INVALID_FORMAT = 2,
  AMBROSIA_XMA2_BAD_PACKET_SIZE = 3,
  AMBROSIA_XMA2_OUT_OF_MEMORY = 4,
  AMBROSIA_XMA2_DECODER_FAILURE = 5,
  AMBROSIA_XMA2_PANIC = 6,
};

int32_t ambrosia_xma2_decoder_create(uint32_t sample_rate, uint32_t channels,
                                    AmbrosiaXma2Decoder **out_decoder);
int32_t ambrosia_xma2_decode_buffer(AmbrosiaXma2Decoder *decoder,
                                   const uint8_t *input, size_t input_len,
                                   AmbrosiaXma2PcmBlock **out_block);
void ambrosia_xma2_decoder_destroy(AmbrosiaXma2Decoder *decoder);
const int16_t *ambrosia_xma2_pcm_data(const AmbrosiaXma2PcmBlock *block);
size_t ambrosia_xma2_pcm_sample_count(const AmbrosiaXma2PcmBlock *block);
void ambrosia_xma2_pcm_destroy(AmbrosiaXma2PcmBlock *block);

#ifdef __cplusplus
}
#endif

#endif
