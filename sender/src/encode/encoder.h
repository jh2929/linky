#pragma once
// Codificación de vídeo y audio. El emisor sondea automáticamente el mejor
// backend disponible: VAAPI → QSV → NVENC → AMF → CPU. La interfaz es única
// para todos los backends.
#include <functional>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/packet.h>
#include <libavutil/frame.h>
}

namespace linky {

enum class Codec { H264, H265, AV1 };

const char* codec_name(Codec c);

struct EncoderInfo {
  std::string backend;          // "vaapi", "qsv", "nvenc", "amf", "cpu-x264"…
  std::vector<Codec> codecs;    // códecs realmente disponibles (probados)
};

// Sondeo hardware: consulta VAAPI/etc y devuelve lo usable en esta máquina.
EncoderInfo probe_encoders();

class VideoEncoder {
 public:
  virtual ~VideoEncoder() = default;

  virtual bool init(Codec codec, int width, int height, int fps,
                    int bitrate_kbps) = 0;
  // Entrada BGR0 (captura) → paquetes Annex B. Devuelve false si hay que
  // descartar el frame (encoder atrasado, política anti-latencia).
  virtual bool encode(AVFrame* bgr0, std::vector<AVPacket*>& out) = 0;
  // Solicitud de keyframe (PLI recibido): reabre el encoder.
  virtual void force_keyframe() = 0;
  virtual bool ready() const = 0;
  virtual const char* backend() const = 0;
};

VideoEncoder* create_video_encoder();

// Opus 48 kHz estéreo. Los fragmentos PCM pueden llegar de cualquier tamaño;
// el encoder acumula frames de 20 ms (960 muestras) internamente.
class AudioEncoder {
 public:
  virtual ~AudioEncoder() = default;
  virtual bool init() = 0;
  // Entrada s16 interleaved; devuelve paquetes Opus (uno por 20 ms).
  virtual bool encode(const int16_t* pcm, int samples, std::vector<AVPacket*>& out) = 0;
  virtual bool ready() const = 0;
};

AudioEncoder* create_audio_encoder();

}  // namespace linky
