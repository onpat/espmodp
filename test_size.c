#include <stdio.h>
#include <stdint.h>

#define XM_DISABLED_EFFECTS 0
#define XM_DISABLED_VOLUME_EFFECTS 0
#define XM_DISABLED_FEATURES 0

#define HAS_VOLUME_COLUMN 1
#define XM_SAMPLE_TYPE int16_t

struct xm_pattern_slot_s {
	uint8_t note;
	uint8_t instrument;
	uint8_t volume_column;
	uint8_t effect_type;
	uint8_t effect_param;
};

int main() {
    printf("sizeof(xm_pattern_slot_s) = %zu\n", sizeof(struct xm_pattern_slot_s));
    return 0;
}
