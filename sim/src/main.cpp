#include "barrier.h"
#include "cloth.h"
#include "gang.h"
#include "viewer.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using Clock = std::chrono::steady_clock;

struct Options {
    int n = 32;
    int threads = 4;
    int steps = 3000;
    int warmup = 300;
    int iterations = 10;
    double dt = 1.0 / 600;
    double stiffness = 2000;
    double bendStiffness = 50;
    double damping = 1.0;
    std::string barrier = "spin";
    std::string csv;
    std::string objDir;
    int objEvery = 0;
    bool view = false;
    bool stepsGiven = false;
    int gang = -1;
    double duration = 0;
    double warmupSec = 1.0;
};

struct SchedStats {
    long switches = 0, voluntary = 0, involuntary = 0, migrations = 0;
};

struct WorkerStats {
    pid_t tid = 0;
    std::vector<double> waitSum, waitMax;
    SchedStats start, end;
};

static double nsSince(Clock::time_point t) { return std::chrono::duration<double, std::nano>(Clock::now() - t).count(); }

static std::pair<int, int> split(int count, int part, int parts) { return {count * part / parts, count * (part + 1) / parts}; }

// Per-thread counters kept by the kernel; they work the same under EEVDF and sched_ext.
static SchedStats readSchedStats() {
    SchedStats s;
    std::ifstream f("/proc/thread-self/sched");
    std::string line;
    char key[64];
    double value;
    while (std::getline(f, line)) {
        if (sscanf(line.c_str(), "%63s : %lf", key, &value) != 2) continue;
        std::string k = key;
        if (k == "nr_switches") s.switches = value;
        if (k == "nr_voluntary_switches") s.voluntary = value;
        if (k == "nr_involuntary_switches") s.involuntary = value;
        if (k == "se.nr_migrations") s.migrations = value;
    }
    return s;
}

static void usage() {
    printf("usage: vbd [--n N] [--threads T] [--steps S] [--warmup W] [--iterations I] [--dt H]\n"
           "           [--stiffness K] [--bend K] [--damping D] [--barrier spin|block]\n"
           "           [--csv FILE] [--obj DIR --obj-every K] [--view] [--gang G]\n"
           "           [--duration SEC [--warmup-sec SEC]]\n"
           "  --view  open a window and run in real time until it is closed (arrows rotate, Esc quits)\n"
           "  --gang G  register the workers as gang G with the gang scheduler (needs scx_gang running and root)\n"
           "  --duration SEC  run for a fixed wall time instead of --steps; the first --warmup-sec (default 1) is not measured\n");
}

static Options parse(int argc, char** argv) {
    Options o;
    for (int a = 1; a < argc; a++) {
        std::string arg = argv[a];
        if (arg == "--view") {
            o.view = true;
            continue;
        }
        if (arg == "-h" || arg == "--help" || a + 1 >= argc) {
            usage();
            exit(arg == "-h" || arg == "--help" ? 0 : 1);
        }
        std::string val = argv[++a];
        if (arg == "--n") o.n = std::stoi(val);
        else if (arg == "--threads") o.threads = std::stoi(val);
        else if (arg == "--steps") o.steps = std::stoi(val), o.stepsGiven = true;
        else if (arg == "--warmup") o.warmup = std::stoi(val);
        else if (arg == "--iterations") o.iterations = std::stoi(val);
        else if (arg == "--dt") o.dt = std::stod(val);
        else if (arg == "--stiffness") o.stiffness = std::stod(val);
        else if (arg == "--bend") o.bendStiffness = std::stod(val);
        else if (arg == "--damping") o.damping = std::stod(val);
        else if (arg == "--barrier") o.barrier = val;
        else if (arg == "--csv") o.csv = val;
        else if (arg == "--obj") o.objDir = val;
        else if (arg == "--obj-every") o.objEvery = std::stoi(val);
        else if (arg == "--gang") o.gang = std::stoi(val);
        else if (arg == "--duration") o.duration = std::stod(val);
        else if (arg == "--warmup-sec") o.warmupSec = std::stod(val);
        else {
            usage();
            exit(1);
        }
    }
    if (o.view && !o.stepsGiven) o.steps = INT_MAX, o.warmup = 0;
    if (o.duration > 0) o.steps = INT_MAX, o.warmup = 0;
    if (o.warmup >= o.steps) o.warmup = 0;
    return o;
}

static double percentile(std::vector<double> v, double p) {
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, (size_t)(p * v.size()))];
}

int main(int argc, char** argv) {
    Options o = parse(argc, argv);
    auto barrier = makeBarrier(o.barrier, o.threads);
    if (!barrier) {
        usage();
        return 1;
    }

    int gangMap = -1;
    if (o.gang >= 0 && (gangMap = openGangMap()) < 0) {
        fprintf(stderr, "cannot open %s: is scx_gang running, and is this run as root?\n", GANG_MAP_PATH);
        return 1;
    }

    Cloth cloth(o.n, 1.0, 0.2, o.stiffness, o.bendStiffness);
    int numColours = cloth.colours.size();
    int measuredSteps = o.steps - o.warmup;
    std::vector<WorkerStats> stats(o.threads);
    std::vector<double> stepNs;
    stepNs.reserve(std::min(measuredSteps, 1 << 20));

    std::unique_ptr<Viewer> viewer;
    if (o.view) viewer = std::make_unique<Viewer>();
    std::atomic<bool> quit{false};
    std::atomic<int> finished{0};
    bool halt = false;
    bool measuring = o.duration == 0 && o.warmup == 0;
    auto runStart = Clock::now();

    auto worker = [&](int id) {
        WorkerStats& ws = stats[id];
        ws.tid = gettid();
        if (gangMap >= 0 && !joinGang(gangMap, o.gang, id)) fprintf(stderr, "thread %d failed to join gang %d\n", ws.tid, o.gang);
        ws.waitSum.assign(numColours, 0);
        ws.waitMax.assign(numColours, 0);
        bool wasMeasured = false;
        auto [lo, hi] = split(cloth.size(), id, o.threads);

        for (int step = 0; step < o.steps; step++) {
            bool measured = measuring;
            if (measured && !wasMeasured) ws.start = readSchedStats();
            wasMeasured = measured;
            auto stepStart = Clock::now();

            cloth.predict(lo, hi, o.dt);
            barrier->wait();

            for (int it = 0; it < o.iterations; it++) {
                for (int c = 0; c < numColours; c++) {
                    const std::vector<int>& verts = cloth.colours[c];
                    auto [a, b] = split(verts.size(), id, o.threads);
                    for (int k = a; k < b; k++) cloth.solveVertex(verts[k], o.dt);

                    auto waitStart = Clock::now();
                    barrier->wait();
                    double waited = nsSince(waitStart);
                    if (measured) {
                        ws.waitSum[c] += waited;
                        ws.waitMax[c] = std::max(ws.waitMax[c], waited);
                    }
                }
            }

            if (id == 0 && o.objEvery > 0 && step % o.objEvery == 0) cloth.writeObj(o.objDir + "/frame_" + std::to_string(step) + ".obj");
            cloth.updateVelocity(lo, hi, o.dt, o.damping);
            if (id == 0) {
                if (o.view) std::this_thread::sleep_until(runStart + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>((step + 1) * o.dt)));
                // Thread 0 decides for everyone; the others read these after the barrier below.
                double elapsed = nsSince(runStart) * 1e-9;
                if (o.duration > 0) {
                    measuring = elapsed >= o.warmupSec;
                    halt = elapsed >= o.warmupSec + o.duration && (measured || !stepNs.empty());
                } else {
                    measuring = step + 1 >= o.warmup;
                }
                halt = halt || quit;
            }
            barrier->wait();

            if (id == 0 && measured) stepNs.push_back(nsSince(stepStart));
            if (halt) break;
        }
        ws.end = readSchedStats();
        finished++;
    };

    std::vector<std::thread> pool;
    for (int id = 0; id < o.threads; id++) pool.emplace_back(worker, id);
    if (viewer) {
        while (finished < o.threads && viewer->draw(cloth)) {
        }
        quit = true;
    }
    for (auto& t : pool) t.join();
    double runSeconds = nsSince(runStart) * 1e-9;
    measuredSteps = stepNs.size();
    if (measuredSteps == 0) {
        fprintf(stderr, "no steps were measured (run too short?)\n");
        return 2;
    }

    double meanStep = 0;
    for (double t : stepNs) meanStep += t;
    meanStep /= stepNs.size();

    printf("VBD cloth %dx%d: %zu springs, %d colours, %d threads, %s barrier, %s\n", o.n, o.n, cloth.springs.size(), numColours, o.threads, o.barrier.c_str(),
           o.gang >= 0 ? ("gang " + std::to_string(o.gang)).c_str() : "no gang");
    printf("%d measured steps, %d iterations, dt=%.4f, wall %.2f s\n\n", measuredSteps, o.iterations, o.dt, runSeconds);
    printf("step time ms: mean %.3f  p50 %.3f  p99 %.3f  max %.3f\n\n", meanStep * 1e-6, percentile(stepNs, 0.50) * 1e-6, percentile(stepNs, 0.99) * 1e-6,
           percentile(stepNs, 1.0) * 1e-6);

    printf("barrier wait per colour (us, over all threads and crossings):\n");
    double crossings = (double)measuredSteps * o.iterations * o.threads;
    double totalWait = 0;
    std::string colourWaits;
    for (int c = 0; c < numColours; c++) {
        double sum = 0, worst = 0;
        for (auto& ws : stats) {
            sum += ws.waitSum[c];
            worst = std::max(worst, ws.waitMax[c]);
        }
        totalWait += sum;
        colourWaits += (c ? "," : "") + std::to_string(sum / crossings * 1e-3);
        printf("  colour %2d  %5zu verts  mean %8.2f  max %10.2f\n", c, cloth.colours[c].size(), sum / crossings * 1e-3, worst * 1e-3);
    }
    double waitPerStep = totalWait / measuredSteps / o.threads;
    printf("  barrier wait per thread per step: %.3f ms (%.1f%% of step time)\n\n", waitPerStep * 1e-6, 100.0 * waitPerStep / meanStep);

    printf("per-thread scheduling (measured steps):\n");
    SchedStats total;
    for (auto& ws : stats) {
        SchedStats d{ws.end.switches - ws.start.switches, ws.end.voluntary - ws.start.voluntary, ws.end.involuntary - ws.start.involuntary,
                     ws.end.migrations - ws.start.migrations};
        printf("  tid %6d  switches %6ld  voluntary %6ld  involuntary %6ld  migrations %6ld\n", ws.tid, d.switches, d.voluntary, d.involuntary, d.migrations);
        total.switches += d.switches;
        total.voluntary += d.voluntary;
        total.involuntary += d.involuntary;
        total.migrations += d.migrations;
    }

    printf("\nphysics: max spring strain %.4f, lowest point y=%.3f m, pinned corner (%.3f, %.3f, %.3f)\n", cloth.maxStrain(), cloth.lowestY(), cloth.x[0].x, cloth.x[0].y,
           cloth.x[0].z);

    printf("RESULT n=%d threads=%d barrier=%s gang=%d colours=%d steps=%d iterations=%d step_mean_ms=%.4f step_p50_ms=%.4f step_p99_ms=%.4f step_max_ms=%.4f "
           "wait_per_step_ms=%.4f switches=%ld voluntary=%ld involuntary=%ld migrations=%ld wall_s=%.3f wait_colour_us=%s\n",
           o.n, o.threads, o.barrier.c_str(), o.gang, numColours, measuredSteps, o.iterations, meanStep * 1e-6, percentile(stepNs, 0.50) * 1e-6,
           percentile(stepNs, 0.99) * 1e-6, percentile(stepNs, 1.0) * 1e-6, waitPerStep * 1e-6, total.switches, total.voluntary, total.involuntary,
           total.migrations, runSeconds, colourWaits.c_str());

    if (!o.csv.empty()) {
        std::ofstream f(o.csv);
        f << "step,step_ms\n";
        for (size_t i = 0; i < stepNs.size(); i++) f << i << "," << stepNs[i] * 1e-6 << "\n";
    }
    return 0;
}
