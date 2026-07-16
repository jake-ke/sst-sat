#ifndef ACTIVITY_HISTOGRAM_H
#define ACTIVITY_HISTOGRAM_H

#include <cstdint>
#include <cstring>
#include <cfloat>
#include <cassert>

// On-chip running histogram of learnt (non-binary) clause activities.
//
// Hardware model: a direct-indexed SRAM array of counters. The bucket index
// is a bit-slice of the activity float32 (8 exponent bits + top MANTISSA_BITS
// mantissa bits) -- pure wiring, no hashing or indirection. Maintained
// incrementally at learn/bump/remove/rescale so reduceDB can find the
// median-activity threshold without touching DRAM. Updates are a few SRAM
// cycles hidden behind the ~100+-cycle DRAM reads that accompany them, so
// they are modeled as zero simulated time; the serial prefix scan at reduce
// time IS modeled (see the reduce_scan self-link in SATSolver).
class ActivityHistogram {
public:
    static const int MANTISSA_BITS = 3;
    static const int NUM_BUCKETS = 1 << (8 + MANTISSA_BITS);  // 2048

    // Rescale multiplies every activity by 2^-RESCALE_EXP (a power of two, so
    // the float exponent shifts by exactly RESCALE_EXP and the histogram
    // update is an exact bucket shift). Values whose biased exponent is
    // <= RESCALE_EXP go denormal/zero: the DRAM sweep flushes them to 0.0f
    // and the histogram collapses their buckets into bucket 0.
    static const int RESCALE_EXP = 66;
    static const int RESCALE_SHIFT = RESCALE_EXP << MANTISSA_BITS;  // 528

    // Activities are non-negative, so the sign bit is ignored.
    static int bucketOf(float act) {
        uint32_t bits;
        std::memcpy(&bits, &act, sizeof(bits));
        return (bits >> (23 - MANTISSA_BITS)) & (NUM_BUCKETS - 1);
    }

    // The exact float transform the rescale sweep applies to every stored
    // activity. Power-of-two multiply is exact for normal results; denormal
    // results are flushed to zero so DRAM ground truth matches the histogram's
    // bucket-0 collapse bit-for-bit.
    static float rescaleValue(float act) {
        float r = act * 0x1p-66f;
        if (r < FLT_MIN) r = 0.0f;  // flush denormals (activities are >= 0)
        return r;
    }

    void add(float act) { count_[bucketOf(act)]++; total_++; }

    void remove(float act) {
        int b = bucketOf(act);
        assert(count_[b] > 0 && total_ > 0);
        count_[b]--;
        total_--;
    }

    // Bump: decrement the old bucket, increment the new. Same-bucket moves
    // cancel (free micro-opt: compare indices, skip both SRAM ops).
    void move(float old_act, float new_act) {
        int ob = bucketOf(old_act), nb = bucketOf(new_act);
        if (ob == nb) return;
        assert(count_[ob] > 0);
        count_[ob]--;
        count_[nb]++;
    }

    // Apply the x2^-RESCALE_EXP bucket shift. Buckets below the first bucket
    // that stays normal (biased exponent RESCALE_EXP+1) collapse into bucket 0.
    void rescaleShift() {
        const int first_normal = RESCALE_SHIFT + (1 << MANTISSA_BITS);
        uint64_t low = 0;
        for (int i = 0; i < first_normal && i < NUM_BUCKETS; i++) low += count_[i];
        for (int i = first_normal; i < NUM_BUCKETS; i++) {
            count_[i - RESCALE_SHIFT] = count_[i];
            count_[i] = 0;
        }
        // Destination buckets [1, 8) are not covered by the shift and bucket 0
        // still holds its pre-shift value (already summed into `low`).
        for (int i = 1; i < first_normal - RESCALE_SHIFT; i++) count_[i] = 0;
        count_[0] = (uint32_t)low;
        // total_ unchanged: every clause is still counted, just rebucketed.
    }

    // Prefix-sum scan: find the smallest bucket b whose cumulative count
    // reaches `target`. Returns b (NUM_BUCKETS if target exceeds the total)
    // and sets `below` = count strictly below bucket b. The tie-bucket quota
    // is target - below. In hardware this is the serial NUM_BUCKETS-cycle
    // scan modeled by the reduce_scan self-link stall.
    int selectThreshold(uint64_t target, uint64_t& below) const {
        uint64_t cum = 0;
        for (int b = 0; b < NUM_BUCKETS; b++) {
            if (cum + count_[b] >= target) { below = cum; return b; }
            cum += count_[b];
        }
        below = cum;
        return NUM_BUCKETS;
    }

    uint64_t total() const { return total_; }

private:
    uint32_t count_[NUM_BUCKETS] = {0};
    uint64_t total_ = 0;
};

#endif // ACTIVITY_HISTOGRAM_H
