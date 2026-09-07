// Minimal linkage check for libopus.
//
// Exists so a broken or mis-cross-compiled dependency fails here, in four lines,
// rather than inside the codec wrapper where the cause would be ambiguous.
#include <opus.h>

#include <cstdio>

int main() {
  int error = OPUS_OK;
  OpusEncoder* encoder = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &error);
  if (encoder == nullptr || error != OPUS_OK) {
    std::printf("encoder_create failed: %s\n", opus_strerror(error));
    return 1;
  }
  OpusDecoder* decoder = opus_decoder_create(48000, 1, &error);
  if (decoder == nullptr || error != OPUS_OK) {
    std::printf("decoder_create failed: %s\n", opus_strerror(error));
    return 1;
  }
  std::printf("opus %s: encoder and decoder created\n", opus_get_version_string());
  opus_encoder_destroy(encoder);
  opus_decoder_destroy(decoder);
  return 0;
}
