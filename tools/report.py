"""把 fifo-dv 导出的 trace.json 渲染成一个自包含的可视化报告。

用法：
    ./fifo-dv --json trace.json --window 240      # 或 --inject-bug
    python tools/report.py trace.json docs/report.html

生成的 HTML 内嵌数据，双击即可打开，不依赖服务器。
"""
import json
import pathlib
import sys

TEMPLATE = r"""<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8">
<title>fifo-dv 验证报告</title>
<style>
:root{--bg:#0f1216;--panel:#161b22;--line:#263041;--ink:#e6edf3;--muted:#8b949e;
      --ok:#3fb950;--bad:#f85149;--sig:#58a6ff;--dim:#30363d}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);
  font:13px/1.6 -apple-system,"Segoe UI","Microsoft YaHei",sans-serif}
header{padding:18px 24px 14px;border-bottom:1px solid var(--line);display:flex;
  align-items:baseline;gap:16px;flex-wrap:wrap}
header h1{margin:0;font-size:15px}
header .cfg{font:12px ui-monospace,Consolas,monospace;color:var(--muted)}
.chips{display:flex;gap:8px;margin-left:auto;flex-wrap:wrap}
.chip{font:11.5px ui-monospace,Consolas,monospace;padding:3px 10px;border-radius:11px;
  border:1px solid var(--line);color:var(--muted)}
.chip b{color:var(--ink);font-weight:600}
.chip.good{border-color:#238636;color:var(--ok)}
.chip.bad{border-color:#6b2020;color:var(--bad)}
main{padding:16px 24px 40px;display:grid;gap:14px}
.card{background:var(--panel);border:1px solid var(--line);border-radius:10px}
.card h2{margin:0;padding:10px 14px;font-size:12.5px;border-bottom:1px solid var(--line);
  display:flex;gap:10px;align-items:baseline;color:var(--muted);font-weight:600}
.card h2 span{font-weight:400;font-size:11.5px}
.body{padding:12px 14px}
canvas{display:block;width:100%}
.legend{display:flex;gap:14px;font-size:11.5px;color:var(--muted);margin-top:8px;flex-wrap:wrap}
.legend i{display:inline-block;width:16px;height:3px;border-radius:2px;margin-right:5px;
  vertical-align:middle}
.covgrid{display:grid;grid-template-columns:repeat(auto-fill,minmax(148px,1fr));gap:8px}
.bin{border:1px solid var(--line);border-radius:7px;padding:7px 9px;font:11.5px ui-monospace,Consolas,monospace}
.bin.hit{border-color:#238636;background:rgba(63,185,80,.08)}
.bin.miss{border-color:#6b2020;background:rgba(248,81,73,.08)}
.bin .n{color:var(--muted)}
.bin .s{float:right;font-weight:600}
.bin.hit .s{color:var(--ok)} .bin.miss .s{color:var(--bad)}
.tip{position:fixed;pointer-events:none;background:#0b0e12;border:1px solid var(--line);
  border-radius:6px;padding:6px 9px;font:11.5px ui-monospace,Consolas,monospace;
  color:var(--ink);display:none;z-index:9;white-space:pre}
.note{font-size:11.5px;color:var(--muted);margin-top:10px;line-height:1.8}
</style></head><body>
<header>
  <h1>fifo-dv · 同步 FIFO 验证报告</h1>
  <span class="cfg" id="cfg"></span>
  <div class="chips" id="chips"></div>
</header>
<main>
  <section class="card">
    <h2>波形 <span id="wavehint"></span></h2>
    <div class="body">
      <canvas id="wave" height="300"></canvas>
      <div class="legend">
        <span><i style="background:var(--sig)"></i>写使能 / 读使能 = 1</span>
        <span><i style="background:var(--bad)"></i>scoreboard 失配拍</span>
        <span><i style="background:var(--dim)"></i>0</span>
      </div>
      <div class="note" id="wavenote"></div>
    </div>
  </section>

  <section class="card">
    <h2>水位（FIFO 内元素个数）<span>容量上限与空/满边界</span></h2>
    <div class="body"><canvas id="occ" height="170"></canvas></div>
  </section>

  <section class="card">
    <h2>功能覆盖率 <span id="covhint"></span></h2>
    <div class="body"><div class="covgrid" id="cov"></div></div>
  </section>
</main>
<div class="tip" id="tip"></div>
<script>
const D = __DATA__;
const $ = s => document.querySelector(s);

$('#cfg').textContent = `depth=${D.config.depth} · cycles=${D.config.cycles} · seed=${D.config.seed}`;
const s = D.summary;
$('#chips').innerHTML = `
  <span class="chip">覆盖 <b>${s.covRandom}%</b> 随机 / <b>${s.covFinal}%</b> 补定向后</span>
  <span class="chip ${s.mismatch ? 'bad' : 'good'}">失配 <b>${s.mismatch}</b></span>
  <span class="chip">满写忽略 <b>${s.ovf}</b></span>
  <span class="chip">空读忽略 <b>${s.udf}</b></span>
  ${D.config.bug ? '<span class="chip bad">已注入 bug</span>' : ''}`;

// ---------- 波形 ----------
const ROWS = [
  {key:'wr', label:'wr_en'},
  {key:'rd', label:'rd_en'},
  {key:'full', label:'full'},
  {key:'empty', label:'empty'},
  {key:'dout', label:'dout(数值)'}
];
const W = D.window;
const cvs = $('#wave'), ctx = cvs.getContext('2d');
const dpr = window.devicePixelRatio || 1;

function drawWave() {
  const cssW = cvs.clientWidth, rowH = 46, padL = 78, padR = 10, top = 10;
  const cssH = top + ROWS.length * rowH + 24;
  cvs.style.height = cssH + 'px';
  cvs.width = cssW * dpr; cvs.height = cssH * dpr;
  ctx.setTransform(dpr,0,0,dpr,0,0);
  ctx.clearRect(0,0,cssW,cssH);
  const stepW = (cssW - padL - padR) / W;

  ROWS.forEach((row, ri) => {
    const y0 = top + ri * rowH;
    const hi = y0 + 12, lo = y0 + 30;
    ctx.fillStyle = '#8b949e'; ctx.font = '11px ui-monospace,Consolas,monospace';
    ctx.fillText(row.label, 8, y0 + 24);
    ctx.strokeStyle = '#21262d'; ctx.beginPath();
    ctx.moveTo(padL, lo + 6); ctx.lineTo(cssW - padR, lo + 6); ctx.stroke();

    if (row.key === 'dout') {
      const max = Math.max(1, ...D.dout);
      D.dout.forEach((v, i) => {
        if (!v) return;
        const x = padL + i * stepW;
        const h = (v / max) * 30;
        ctx.fillStyle = 'rgba(88,166,255,.55)';
        ctx.fillRect(x + 0.5, lo + 4 - h, Math.max(1.2, stepW - 0.8), h);
      });
      return;
    }

    let prev = null;
    const vals = D[row.key];
    for (let i = 0; i < vals.length; i++) {
      const v = vals[i] ? 1 : 0;
      const x = padL + i * stepW;
      const y = v ? hi : lo;
      if (prev === null || prev !== v) {
        if (prev !== null) { ctx.strokeStyle = '#58a6ff'; ctx.beginPath();
          ctx.moveTo(x, prev ? hi : lo); ctx.lineTo(x, y); ctx.stroke(); }
      }
      ctx.strokeStyle = '#58a6ff'; ctx.lineWidth = 1.6;
      ctx.beginPath(); ctx.moveTo(x, y); ctx.lineTo(x + stepW, y); ctx.stroke();
      prev = v;
    }
  });

  // 失配竖条
  D.mis.forEach((m, i) => {
    if (!m) return;
    const x = padL + i * stepW;
    ctx.fillStyle = 'rgba(248,81,73,.22)';
    ctx.fillRect(x, top, Math.max(1.5, stepW), ROWS.length * rowH);
  });

  // 刻度
  ctx.fillStyle = '#6e7681'; ctx.font = '10.5px ui-monospace,Consolas,monospace';
  for (let i = 0; i <= 6; i++) {
    const cyc = Math.round(i * W / 6);
    const x = padL + cyc * stepW;
    ctx.fillText(String(cyc), x - 6, cssH - 6);
  }
}

// 悬停提示
const tip = $('#tip');
cvs.addEventListener('mousemove', e => {
  const r = cvs.getBoundingClientRect();
  const padL = 78, padR = 10, stepW = (r.width - padL - padR) / W;
  const i = Math.floor((e.clientX - r.left - padL) / stepW);
  if (i < 0 || i >= W) { tip.style.display = 'none'; return; }
  tip.style.display = 'block';
  tip.style.left = (e.clientX + 14) + 'px';
  tip.style.top = (e.clientY + 14) + 'px';
  tip.textContent = `cycle ${i}\nwr=${D.wr[i]} rd=${D.rd[i]}\nfull=${D.full[i]} empty=${D.empty[i]}\ncnt=${D.cnt[i]} dout=${D.dout[i]}` +
    (D.mis[i] ? '\n⚠ 失配' : '');
});
cvs.addEventListener('mouseleave', () => tip.style.display = 'none');

// ---------- 水位 ----------
const occ = $('#occ'), octx = occ.getContext('2d');
function drawOcc() {
  const cssW = occ.clientWidth, cssH = 170, padL = 40, padR = 12, top = 12, bot = 24;
  occ.style.height = cssH + 'px';
  occ.width = cssW * dpr; occ.height = cssH * dpr;
  octx.setTransform(dpr,0,0,dpr,0,0);
  octx.clearRect(0,0,cssW,cssH);
  const innerW = cssW - padL - padR, innerH = cssH - top - bot;
  const maxC = Math.max(D.config.depth, ...D.cnt);
  const stepW = innerW / W;

  octx.strokeStyle = '#21262d';
  for (let g = 0; g <= 4; g++) {
    const y = top + innerH * g / 4;
    octx.beginPath(); octx.moveTo(padL, y); octx.lineTo(cssW - padR, y); octx.stroke();
    octx.fillStyle = '#6e7681'; octx.font = '10.5px ui-monospace,Consolas,monospace';
    octx.fillText(String(Math.round(maxC * (1 - g / 4))), 8, y + 4);
  }
  // 满线
  const yFull = top + innerH * (1 - D.config.depth / maxC);
  octx.strokeStyle = 'rgba(248,81,73,.5)'; octx.setLineDash([4,4]);
  octx.beginPath(); octx.moveTo(padL, yFull); octx.lineTo(cssW - padR, yFull); octx.stroke();
  octx.setLineDash([]);

  octx.beginPath();
  D.cnt.forEach((c, i) => {
    const x = padL + i * stepW, y = top + innerH * (1 - c / maxC);
    i ? octx.lineTo(x, y) : octx.moveTo(x, y);
  });
  octx.strokeStyle = '#58a6ff'; octx.lineWidth = 1.6; octx.stroke();
  octx.lineTo(padL + (W - 1) * stepW, top + innerH);
  octx.lineTo(padL, top + innerH); octx.closePath();
  octx.fillStyle = 'rgba(88,166,255,.16)'; octx.fill();
}

// ---------- 覆盖率 ----------
$('#covhint').textContent = `${D.coverage.filter(c=>c.hit).length}/${D.coverage.length} 命中`;
$('#cov').innerHTML = D.coverage.map(c =>
  `<div class="bin ${c.hit?'hit':'miss'}"><span class="n">${c.name}</span>
     <span class="s">${c.hit?'hit':'MISS'}</span></div>`).join('');

const missBins = D.coverage.filter(c => !c.hit).map(c => c.name);
$('#wavenote').textContent = D.mis.some(Boolean)
  ? `前 ${W} 拍里出现失配，红色竖条标出位置；把鼠标移到波形上看逐拍信号。`
  : `前 ${W} 拍无失配。${missBins.length ? '随机阶段未覆盖：' + missBins.join('、') + '（靠定向用例补）' : '覆盖率已满'}`;
$('#wavehint').textContent = `前 ${W} 拍`;

function redraw() { drawWave(); drawOcc(); }
redraw();
window.addEventListener('resize', redraw);
</script></body></html>"""


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    src, dst = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    data = json.loads(src.read_text(encoding="utf-8"))
    dst.parent.mkdir(parents=True, exist_ok=True)
    html = TEMPLATE.replace("__DATA__", json.dumps(data, ensure_ascii=False))
    dst.write_text(html, encoding="utf-8")
    print(f"报告已生成：{dst}  ({len(html)//1024} KB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
