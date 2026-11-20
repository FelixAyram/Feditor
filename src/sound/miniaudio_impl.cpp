// Single translation unit that compiles miniaudio (decoders + engine). Everything else includes
// "miniaudio/miniaudio.h" for declarations only. Encoders and procedural generators are not needed.
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio/miniaudio.h"
