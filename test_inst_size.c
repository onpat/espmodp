#include <stdio.h>
#include <stdint.h>

#define HAS_FEATURE_MULTISAMPLE_INSTRUMENTS 1
#define MAX_NOTE 96
#define MAX_ENVELOPE_POINTS 12

struct xm_envelope_point_s {
	uint16_t frame;
	uint16_t value;
};
typedef struct xm_envelope_point_s xm_envelope_point_t;

struct xm_envelope_s {
	xm_envelope_point_t points[MAX_ENVELOPE_POINTS];
	uint8_t num_points;
	uint8_t sustain_point;
	uint8_t loop_start_point;
	uint8_t loop_end_point;
};
typedef struct xm_envelope_s xm_envelope_t;

struct xm_instrument_s {
	xm_envelope_t volume_envelope;
	xm_envelope_t panning_envelope;
	uint16_t volume_fadeout;
	uint16_t sample_of_notes[MAX_NOTE];
	uint8_t vibrato_type;
	uint8_t vibrato_sweep;
	uint8_t vibrato_depth;
	uint8_t vibrato_rate;
};

int main() {
    printf("sizeof(xm_instrument_s) = %zu\n", sizeof(struct xm_instrument_s));
    return 0;
}
