// mini DV 平台：被测逻辑是同步 FIFO，验证平台按 UVM 的分层复现一遍
//   generator(CRV) → driver → DUT → monitor → scoreboard → coverage
// 不依赖任何 EDA 工具，全部软件实现，目的是把验证方法学本身讲清楚。

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

// ---------------- DUT：同步 FIFO ----------------
struct Fifo {
    bool bug = false;   // 注入 bug：满的时候仍然写，用来验证平台能不能抓到
    int depth;
    std::vector<int> mem;
    int wr = 0, rd = 0, count = 0;
    bool overflow = false, underflow = false;

    explicit Fifo(int d) : depth(d), mem(d, 0) {}

    // 一个时钟沿：同时给出读写信号
    void tick(bool wr_en, bool rd_en, int din, int& dout, bool& full, bool& empty) {
        dout = 0;
        full = (count == depth);
        empty = (count == 0);

        bool do_read = rd_en && !empty;              // 空读被忽略（记 underflow）
        if (rd_en && empty) underflow = true;
        bool do_write = wr_en && (!full || bug);      // 满写被忽略（记 overflow）；注入 bug 时不忽略
        if (wr_en && full) overflow = true;

        if (do_read) {
            dout = mem[rd];
            rd = (rd + 1) % depth;
            count--;
        }
        if (do_write) {
            mem[wr] = din;
            wr = (wr + 1) % depth;
            count++;
        }
        full = (count == depth);
        empty = (count == 0);
    }
};

// ---------------- 参考模型：和 DUT 独立实现（不能拿 DUT 当期望） ----------------
struct RefModel {
    int depth;
    std::vector<int> q;
    explicit RefModel(int d) : depth(d) {}

    // 和 RTL 的同拍读写语义保持一致：先读后写
    void step(bool wr_en, bool rd_en, int din, int& dout, bool& full, bool& empty) {
        dout = 0;
        full = ((int)q.size() == depth);
        empty = q.empty();
        if (rd_en && !q.empty()) {
            dout = q.front();
            q.erase(q.begin());
        }
        if (wr_en && (int)q.size() < depth) q.push_back(din);
        full = ((int)q.size() == depth);
        empty = q.empty();
    }
};

// ---------------- 覆盖率：功能覆盖点 + 交叉覆盖 ----------------
struct Coverage {
    std::map<std::string, std::pair<int, int>> bins;  // name -> {hit, total}
    void def(const std::string& n, int total) { bins[n] = {0, total}; }
    void hit(const std::string& n) { if (bins.count(n)) bins[n].first++; }
    void hitBin(const std::string& n, int idx) {
        // idx 用于把「命中第几个 bin」摊平成一行，便于统计
        char buf[128];
        snprintf(buf, sizeof(buf), "%s[%d]", n.c_str(), idx);
        hit(buf);
    }
    double percent() const {
        int h = 0, t = 0;
        for (auto& kv : bins) { if (kv.second.first > 0) h++; t++; }
        return t ? 100.0 * h / t : 0;
    }
    std::vector<std::string> missed() const {
        std::vector<std::string> m;
        for (auto& kv : bins) if (kv.second.first == 0) m.push_back(kv.first);
        return m;
    }
};

static const int LEVELS = 4;   // 水位：0=空 1=低 2=高 3=满
static const int NUM_RANGES = 4;  // din: 零/正小/正大/负

static void initCoverage(Coverage& c) {
    const char* simple[] = {"wr_only", "rd_only", "simul_rw", "idle"};
    for (auto n : simple) c.def(n, 1);
    for (int i = 0; i < LEVELS; ++i) c.def("level[" + std::to_string(i) + "]", 1);
    for (int i = 0; i < NUM_RANGES; ++i) c.def("din_range[" + std::to_string(i) + "]", 1);
    c.def("x_full_wr[0]", 1);
    c.def("x_empty_rd[0]", 1);
    c.def("b2b_write[0]", 1);
    c.def("b2b_read[0]", 1);
    c.def("wrap_around[0]", 1);
}

// 数据边界：0 / 小值 / 大值 / 全 1（255 最容易暴露位宽截断）
static int dinRange(int v) {
    if (v == 0) return 0;
    if (v > 0 && v < 128) return 1;
    if (v < 255) return 2;
    return 3;
}

static void sampleCoverage(Coverage& c, bool wr, bool rd, int din, int count, int depth) {
    if (wr && rd) c.hit("simul_rw");
    else if (wr) c.hit("wr_only");
    else if (rd) c.hit("rd_only");
    else c.hit("idle");

    int lv = 0;
    if (count == 0) lv = 0;
    else if (count == depth) lv = 3;
    else if (count * 2 < depth) lv = 1;
    else lv = 2;
    c.hitBin("level", lv);
    c.hitBin("din_range", dinRange(din));

    if (wr && count == depth) c.hitBin("x_full_wr", 0);
    if (rd && count == 0) c.hitBin("x_empty_rd", 0);
}

// ---------------- 结果 ----------------
struct Result {
    long cycles = 0, mismatches = 0, wrCycles = 0, rdCycles = 0;
    long ovf = 0, udf = 0;
    std::string firstMismatch;
};

static int g_depth = 8;

// 单拍执行 + 比对（DUT 与参考模型并行推进）
static inline int stepOne(Fifo& dut, RefModel& ref, bool wr, bool rd, int din, Result& r,
                          Coverage* cov, bool prevWr, bool prevRd, bool* bad = nullptr) {
    int doutDut = 0, doutRef = 0;
    bool fullD, emptyD, fullR, emptyR;
    int cntBefore = dut.count;

    dut.tick(wr, rd, din, doutDut, fullD, emptyD);
    ref.step(wr, rd, din, doutRef, fullR, emptyR);

    r.cycles++;
    if (wr) r.wrCycles++;
    if (rd) r.rdCycles++;
    if (dut.overflow) { r.ovf++; dut.overflow = false; }
    if (dut.underflow) { r.udf++; dut.underflow = false; }

    // scoreboard：数据、状态标志、水位三者都要比
    bool badFlag = (doutDut != doutRef) || (fullD != fullR) || (emptyD != emptyR) ||
                   (dut.count != (int)ref.q.size());
    if (badFlag) {
        if (r.mismatches == 0) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                     "cycle %ld: wr=%d rd=%d din=%d | dut(dout=%d full=%d empty=%d cnt=%d) "
                     "ref(dout=%d full=%d empty=%d cnt=%d)",
                     r.cycles, wr, rd, din, doutDut, fullD, emptyD, dut.count,
                     doutRef, fullR, emptyR, (int)ref.q.size());
            r.firstMismatch = buf;
        }
        r.mismatches++;
    }
    if (bad) *bad = badFlag;

    if (cov) {
        sampleCoverage(*cov, wr, rd, din, cntBefore, g_depth);
        if (wr && prevWr) cov->hitBin("b2b_write", 0);
        if (rd && prevRd) cov->hitBin("b2b_read", 0);
        if (rd && cntBefore == g_depth) cov->hitBin("wrap_around", 0);
    }
    return doutDut;
}

// ---------------- 约束随机激励 ----------------
struct Gen {
    std::mt19937 rng;
    std::uniform_int_distribution<int> rnd{0, 99};
    std::uniform_int_distribution<int> din{0, 255};
    int idleRun = 0;

    explicit Gen(uint32_t seed) : rng(seed) {}

    // 约束：连续 idle 不超过 2 拍；随机权重偏向让水位来回走
    void next(int count, int depth, bool& wr, bool& rd, int& d) {
        int roll = rnd(rng);
        wr = rd = false;
        d = din(rng);

        if (idleRun >= 2) {                       // 约束：不允许长时间空转
            roll = 10 + (roll % 40);
        }
        if (count == 0) {                          // 空时读没有意义，把权重挪给写
            wr = (roll < 85);
            rd = false;
            idleRun = wr ? 0 : idleRun + 1;
        } else if (count == depth) {               // 满时写没有意义，把权重挪给读
            rd = (roll < 80);
            wr = (roll >= 80);
            idleRun = (wr || rd) ? 0 : idleRun + 1;
        } else {
            if (roll < 45) wr = true;
            else if (roll < 75) rd = true;
            else if (roll < 95) { wr = true; rd = true; }   // 同拍读写最容易出 bug
            else idleRun++;
            if (wr || rd) idleRun = 0;
        }
    }
};

static void printCoverage(const char* title, Coverage& c) {
    printf("\n--- %s：覆盖率 %.1f%% ---\n", title, c.percent());
    for (auto& kv : c.bins) {
        printf("  %-16s %s\n", kv.first.c_str(), kv.second.first > 0 ? "hit" : "MISS");
    }
}

// 严格解析整数参数。以前全是裸 atoi："--depth abc" 静默变 0，于是 FIFO 深度 0、
// 读永远不成功，平台照样打印 "mismatch 0 / 完全一致"——什么都没测却像满分；
// "--depth -3" 更直接在 std::vector 构造时抛 length_error 崩掉。参数写错必须立刻退 2，
// 否则一个假通过的回归比失败更危险。
static bool parseInt(const char* name, const char* s, long long lo, long long hi, long long& out) {
    char* end = nullptr;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    if (end == s || *end != '\0' || errno == ERANGE || v < lo || v > hi) {
        fprintf(stderr, "error: %s 需要 [%lld, %lld] 内的整数（当前 \"%s\"）\n", name, lo, hi, s);
        return false;
    }
    out = v;
    return true;
}

int main(int argc, char** argv) {
    uint32_t seed = 20261116;
    bool injectBug = false;
    std::string jsonPath;
    std::string covJsonPath;
    int window = 240;
    long cycles = 20000;
    int depth = 8;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const bool hasVal = i + 1 < argc && argv[i + 1][0] != '\0';
        long long v = 0;
        bool ok = true;
        if (!strcmp(a, "--inject-bug")) { injectBug = true; continue; }
        if (!hasVal) { fprintf(stderr, "error: %s 缺少参数（或参数名写错）\n", a); ok = false; }
        else if (!strcmp(a, "--json")) jsonPath = argv[++i];
        else if (!strcmp(a, "--cov-json")) covJsonPath = argv[++i];
        else if (!strcmp(a, "--seed")) { ok = parseInt(a, argv[++i], 0, 4294967295LL, v); seed = (uint32_t)v; }
        else if (!strcmp(a, "--cycles")) { ok = parseInt(a, argv[++i], 1, 10000000LL, v); cycles = (long)v; }
        else if (!strcmp(a, "--depth")) { ok = parseInt(a, argv[++i], 1, 4096, v); depth = (int)v; }
        else if (!strcmp(a, "--window")) { ok = parseInt(a, argv[++i], 1, 100000, v); window = (int)v; }
        else { fprintf(stderr, "error: 未知参数 \"%s\"\n", a); ok = false; }
        if (!ok) {
            fprintf(stderr,
                    "usage: fifo_dv [--depth 1..4096] [--cycles 1..10000000] [--seed 0..2^32-1]\n"
                    "               [--window 1..100000] [--inject-bug] [--json FILE] [--cov-json FILE]\n");
            return 2;
        }
    }
    g_depth = depth;

    printf("=== mini DV：同步 FIFO 随机验证 ===\n");
    printf("配置：depth=%d  cycles=%ld  seed=%u%s\n", depth, cycles, seed,
           injectBug ? "  [已注入 bug：满写不忽略]" : "");

    Fifo dut(depth);
    dut.bug = injectBug;
    RefModel ref(depth);
    Gen gen(seed);
    Result res;
    Coverage cov;
    initCoverage(cov);

    bool prevWr = false, prevRd = false;
    std::vector<int> sWr, sRd, sFull, sEmpty, sCnt, sDout, sMis;
    for (long i = 0; i < cycles; ++i) {
        bool wr, rd; int din;
        gen.next(dut.count, depth, wr, rd, din);
        bool bad = false;
        int dout = stepOne(dut, ref, wr, rd, din, res, &cov, prevWr, prevRd, &bad);
        if ((long)sWr.size() < window) {
            sWr.push_back(wr); sRd.push_back(rd);
            sFull.push_back(dut.count == depth); sEmpty.push_back(dut.count == 0);
            sCnt.push_back(dut.count); sDout.push_back(dout);
            sMis.push_back(bad ? 1 : 0);
        }
        prevWr = wr; prevRd = rd;
    }

    printf("\n随机跑完：%ld 拍，写 %ld 次、读 %ld 次，溢出忽略 %ld 次、空读忽略 %ld 次\n",
           res.cycles, res.wrCycles, res.rdCycles, res.ovf, res.udf);
    printf("scoreboard：mismatch %ld 处%s\n", res.mismatches,
           res.mismatches ? "（见下）" : "，DUT 与参考模型完全一致");
    if (res.mismatches) printf("  首个失配 -> %s\n", res.firstMismatch.c_str());
    printCoverage("随机激励", cov);
    const double covRandom = cov.percent();
    std::vector<std::pair<std::string, bool>> covSnap;   // 随机阶段快照，和 94.1% 对得上
    for (auto& kv : cov.bins) covSnap.push_back({kv.first, kv.second.first > 0});
    const size_t missRandom = cov.missed().size();

    // ---------- 定向用例：补随机没打到的地方 ----------
    auto missed = cov.missed();
    printf("\n覆盖率缺口 %zu 项，启动定向用例补齐：\n", missed.size());
    for (auto& m : missed) printf("  - %s\n", m.c_str());

    // 定向 1：把水位推到满，再在满的时候连续写两拍（x_full_wr + b2b_write）
    {
        Fifo d2(depth); RefModel r2(depth); Result r3;
        for (int i = 0; i < depth; ++i) stepOne(d2, r2, true, false, 100 + i, r3, nullptr, false, false);
        stepOne(d2, r2, true, false, 200, r3, &cov, false, false);
        stepOne(d2, r2, true, false, 201, r3, &cov, true, false);
        res.mismatches += r3.mismatches;
    }
    // 定向 2：空的时候连续读两拍（x_empty_rd + b2b_read）
    {
        Fifo d2(depth); RefModel r2(depth); Result r3;
        stepOne(d2, r2, false, true, 0, r3, &cov, false, false);
        stepOne(d2, r2, false, true, 0, r3, &cov, false, true);
        res.mismatches += r3.mismatches;
    }
    // 定向 3：写满后读走一个再写（回绕 + 边界 din）
    {
        Fifo d2(depth); RefModel r2(depth); Result r3;
        for (int i = 0; i < depth; ++i) stepOne(d2, r2, true, false, 255, r3, nullptr, false, false);
        stepOne(d2, r2, false, true, 0, r3, &cov, false, false);          // wrap_around
        stepOne(d2, r2, true, false, 0, r3, &cov, false, false);          // din_range=0
        stepOne(d2, r2, true, false, 255, r3, &cov, true, false);         // din_range=3
        res.mismatches += r3.mismatches;
    }

    printCoverage("补定向后", cov);

    if (!jsonPath.empty()) {
        FILE* f = fopen(jsonPath.c_str(), "w");
        if (f) {
            fprintf(f, "{\n\"config\":{\"depth\":%d,\"cycles\":%ld,\"seed\":%u,\"bug\":%s},\n",
                    depth, cycles, seed, injectBug ? "true" : "false");
            fprintf(f, "\"summary\":{\"mismatch\":%ld,\"ovf\":%ld,\"udf\":%ld,\"covRandom\":%.1f,\"covFinal\":%.1f,\"missing\":\"%s\"},\n",
                    res.mismatches, res.ovf, res.udf, covRandom, cov.percent(),
                    missRandom ? "x_empty_rd" : "");
            fprintf(f, "\"window\":%zu,\n", sWr.size());
            auto dumpArr = [&](const char* k, std::vector<int>& v, bool last) {
                fprintf(f, "\"%s\":[", k);
                for (size_t i = 0; i < v.size(); ++i) fprintf(f, "%d%s", v[i], i + 1 < v.size() ? "," : "");
                fprintf(f, "]%s\n", last ? "" : ",");
            };
            dumpArr("wr", sWr, false);
            dumpArr("rd", sRd, false);
            dumpArr("full", sFull, false);
            dumpArr("empty", sEmpty, false);
            dumpArr("cnt", sCnt, false);
            dumpArr("dout", sDout, false);
            dumpArr("mis", sMis, true);
            fprintf(f, ",\"coverage\":[");
            bool first = true;
            for (auto& kv : covSnap) {
                fprintf(f, "%s{\"name\":\"%s\",\"hit\":%s}", first ? "" : ",",
                        kv.first.c_str(), kv.second ? "true" : "false");
                first = false;
            }
            fprintf(f, "]\n}\n");
            fclose(f);
            printf("\n已写出 trace JSON：%s\n", jsonPath.c_str());
        }
    }

    // 覆盖率单独落一份 JSON：只关心「哪些 bin 命中」，体积小，方便多 seed 跑完做趋势对比
    if (!covJsonPath.empty()) {
        FILE* cf = fopen(covJsonPath.c_str(), "w");
        if (cf) {
            fprintf(cf, "{\"seed\":%u,\"depth\":%d,\"cycles\":%ld,\"covRandom\":%.1f,\"covFinal\":%.1f,\"bins\":{",
                    seed, depth, cycles, covRandom, cov.percent());
            bool firstB = true;
            for (auto& kv : cov.bins) {
                fprintf(cf, "%s\"%s\":%d", firstB ? "" : ",", kv.first.c_str(), kv.second.first);
                firstB = false;
            }
            // 随机阶段快照：能看出"哪个 bin 是随机没打到、靠定向补的"
            fprintf(cf, "},\"randomBins\":{");
            bool firstR = true;
            for (auto& kv : covSnap) {
                fprintf(cf, "%s\"%s\":%d", firstR ? "" : ",", kv.first.c_str(), kv.second ? 1 : 0);
                firstR = false;
            }
            fprintf(cf, "}}\n");
            fclose(cf);
            printf("\n已写出覆盖率 JSON：%s\n", covJsonPath.c_str());
        }
    }

    printf("\n=== 结论 ===\n");
    printf("随机 %ld 拍覆盖 %.1f%%（缺 %zu 项）；补 3 个定向用例后 %.1f%%\n",
           cycles, covRandom, missRandom, cov.percent());
    printf("scoreboard 全程 mismatch：%ld\n", res.mismatches);
    return res.mismatches ? 1 : 0;
}
