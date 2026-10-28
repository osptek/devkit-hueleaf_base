/* Six-ink browser dithering and the OTP full-frame transport. */
(function (root, factory) {
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.EinkPipeline = api;
})(typeof globalThis !== 'undefined' ? globalThis : this, function () {
  'use strict';
  const WIDTH = 480, HEIGHT = 720, FRAME_BYTES = WIDTH * HEIGHT / 2;
  const FORMAT = 'otp6-v1';
  let panelRotation = 0;
  function setPanelRotation(degrees) {
    if (degrees !== 0 && degrees !== 180) throw new Error('不支持的整屏安装方向');
    panelRotation = degrees;
  }
  function sourceIndex(x, y) {
    return panelRotation === 180 ? (HEIGHT - 1 - y) * WIDTH + WIDTH - 1 - x : y * WIDTH + x;
  }

  const COLORS = Object.freeze([
    { id: 0, name: '黑', css: '#111412' },
    { id: 1, name: '白', css: '#f4f3e8' },
    { id: 2, name: '黄', css: '#e7c83f' },
    { id: 3, name: '红', css: '#c63b35' },
    { id: 5, name: '蓝', css: '#315eae' },
    { id: 6, name: '绿', css: '#398451' },
  ].map(Object.freeze));
  const clamp = (x, lo, hi) => Math.max(lo, Math.min(hi, x));
  const linear = Float32Array.from({ length: 256 }, (_, i) => {
    const v = i / 255;
    return v <= 0.04045 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4;
  });
  const palette = COLORS.map(c => {
    const rgb = [1, 3, 5].map(i => parseInt(c.css.slice(i, i + 2), 16));
    const lin = rgb.map(v => linear[v]);
    return { ...c, rgb, lin, luminance: .2126 * lin[0] + .7152 * lin[1] + .0722 * lin[2] };
  });
  const validInk = id => Number.isInteger(id) && (id === 0 || id === 1 || id === 2 || id === 3 || id === 5 || id === 6);
  const MODES = Object.freeze({ fast: .55, standard: .85, best: 1 });
  const canceledError = () => Object.assign(new Error('预览已更新'), { name: 'AbortError' });

  // Project unrepresentable source colors to the six-ink linear RGB hull.
  // This removes impossible residuals BEFORE diffusion; representable mixtures
  // retain full error gain. Coordinates use the same weights as ink selection.
  function sixInkHull(style) {
    const w=(style==='illustration'?[1,1,1]:[.3,.5,.2]).map(Math.sqrt);
    const points=palette.map(c=>c.lin.map((v,i)=>v*w[i])),faces=[];
    for(let i=0;i<6;i++)for(let j=i+1;j<6;j++)for(let k=j+1;k<6;k++) {
      const a=points[i],b=points[j],c=points[k],u=b.map((v,n)=>v-a[n]),v=c.map((v,n)=>v-a[n]);
      let normal=[u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]];
      const norm=Math.hypot(...normal);if(norm<1e-10)continue;
      normal=normal.map(v=>v/norm);
      const sides=points.map(p=>normal.reduce((sum,v,n)=>sum+v*(p[n]-a[n]),0));
      if(Math.min(...sides)<-1e-8&&Math.max(...sides)>1e-8)continue;
      if(Math.max(...sides)>1e-8)normal=normal.map(v=>-v);
      const dot=(a,b)=>a.reduce((sum,v,i)=>sum+v*b[i],0);
      const uu=dot(u,u),uv=dot(u,v),vv=dot(v,v);
      const edges=[[a,b],[b,c],[c,a]].map(([a,b])=>{const d=b.map((v,i)=>v-a[i]);return {a,d,n:dot(d,d)};});
      faces.push({a,u,v,normal,uu,uv,vv,det:uu*vv-uv*uv,edges});
    }
    return {w,faces};
  }
  function projectSixColor(r,g,b,hull,out) {
    const x=r*hull.w[0],y=g*hull.w[1],z=b*hull.w[2];
    let outside=false;
    for(const f of hull.faces) {
      if(f.normal[0]*(x-f.a[0])+f.normal[1]*(y-f.a[1])+f.normal[2]*(z-f.a[2])>1e-8){outside=true;break;}
    }
    if(!outside){out[0]=r;out[1]=g;out[2]=b;return;}
    let distance=Infinity,bx=x,by=y,bz=z;
    for(const f of hull.faces) {
      const h=f.normal[0]*(x-f.a[0])+f.normal[1]*(y-f.a[1])+f.normal[2]*(z-f.a[2]);
      const qx=x-h*f.normal[0],qy=y-h*f.normal[1],qz=z-h*f.normal[2];
      const tx=qx-f.a[0],ty=qy-f.a[1],tz=qz-f.a[2];
      const tu=tx*f.u[0]+ty*f.u[1]+tz*f.u[2],tv=tx*f.v[0]+ty*f.v[1]+tz*f.v[2];
      const s=(tu*f.vv-tv*f.uv)/f.det,t=(tv*f.uu-tu*f.uv)/f.det;
      if(s>=0&&t>=0&&s+t<=1) {
        const d=(x-qx)**2+(y-qy)**2+(z-qz)**2;
        if(d<distance){distance=d;bx=qx;by=qy;bz=qz;}
      } else for(const e of f.edges) {
        const u=clamp(((x-e.a[0])*e.d[0]+(y-e.a[1])*e.d[1]+(z-e.a[2])*e.d[2])/e.n,0,1);
        const qx=e.a[0]+u*e.d[0],qy=e.a[1]+u*e.d[1],qz=e.a[2]+u*e.d[2];
        const d=(x-qx)**2+(y-qy)**2+(z-qz)**2;
        if(d<distance){distance=d;bx=qx;by=qy;bz=qz;}
      }
    }
    out[0]=bx/hull.w[0];out[1]=by/hull.w[1];out[2]=bz/hull.w[2];
  }
  // Source-domain bilateral filter: 3x3, range-weighted, before quantization.
  // One browser-side RGB row, no frame buffer on the MCU. Do not filter IDs:
  // removing output dots would destroy the deliberate spatial color mixture.
  function sixColorRow(rgba, y, row, range) {
    for (let x=0;x<WIDTH;x++) {
      const center=(y*WIDTH+x)*4;
      let r=0,g=0,b=0,total=0;
      for(let dy=-1;dy<=1;dy++) {
        const yy=y+dy;if(yy<0||yy>=HEIGHT)continue;
        for(let dx=-1;dx<=1;dx++) {
          const xx=x+dx;if(xx<0||xx>=WIDTH)continue;
          const j=(yy*WIDTH+xx)*4;
          const delta=Math.max(Math.abs(rgba[j]-rgba[center]),Math.abs(rgba[j+1]-rgba[center+1]),
            Math.abs(rgba[j+2]-rgba[center+2]),Math.abs(rgba[j+3]-rgba[center+3]));
          const w=range[delta]*(dx===0?2:1)*(dy===0?2:1);
          if(!w)continue;
          const a=rgba[j+3]/255;
          r+=w*(linear[rgba[j]]*a+1-a);g+=w*(linear[rgba[j+1]]*a+1-a);b+=w*(linear[rgba[j+2]]*a+1-a);total+=w;
        }
      }
      row[x*3]=r/total;row[x*3+1]=g/total;row[x*3+2]=b/total;
    }
  }
  // Only two error rows are kept. Full-strength linear-light diffusion preserves
  // the mean ink coverage for gray, pink and other spatially mixed colors.
  function* quantizeSteps(rgba, options = {}) {
    const { gray = false, quality = 'best', style = 'photo', canceled = () => false } = options;
    if (!Object.hasOwn(MODES, quality)) throw new Error('未知混色质量');
    if (!['photo', 'illustration', 'flat'].includes(style)) throw new Error('未知呈现风格');
    if (!rgba || rgba.length !== WIDTH * HEIGHT * 4) throw new Error('裁剪图尺寸必须为 480×720');
    const denoise = options.denoise ??'off';
    if (!['off','balanced','strong'].includes(denoise)) throw new Error('未知六色降噪等级');
    const clean = style !== 'flat' && denoise !== 'off';
    const strong = denoise === 'strong';
    const filtered = clean ? new Float32Array(WIDTH*3) : null;
    const hull = clean && !gray ? sixInkHull(style) : null, projected = clean ? new Float64Array(3) : null;
    const range = clean ? Float32Array.from({length:256},(_,d)=>d>(strong?48:32)?0:Math.exp(-d*d/(2*(strong?20:12)**2))) : null;
    const inks = gray ? palette.slice(0, 2) : palette;
    const mono = gray;
    const gain = style === 'flat' ? 0 : (gray ? 1 : MODES[quality]);
    const ids = new Uint8Array(WIDTH * HEIGHT);
    const preview = new Uint8ClampedArray(ids.length * 4);
    let current = gain ? new Float32Array((WIDTH + 2) * 3) : null;
    let next = gain ? new Float32Array(current.length) : null;
    for (let y = 0; y < HEIGHT; y++) {
      if (canceled()) throw canceledError();
      if(clean) sixColorRow(rgba,y,filtered,range);
      const dir = y & 1 ? -1 : 1;
      for (let step = 0; step < WIDTH; step++) {
        const x = dir === 1 ? step : WIDTH - 1 - step;
        const i = y * WIDTH + x, e = (x + 1) * 3, src = i * 4;
        const alpha = rgba[src + 3] / 255;
        let r = linear[rgba[src]] * alpha + (1 - alpha);
        let g = linear[rgba[src + 1]] * alpha + (1 - alpha);
        let b = linear[rgba[src + 2]] * alpha + (1 - alpha);
        if(clean) {r=filtered[x*3];g=filtered[x*3+1];b=filtered[x*3+2];}
        // A neutral source must not become a complementary yellow/blue pattern.
        // Gate by SOURCE color, not error-perturbed color, to avoid oscillation.
        const encode=v=>255*(v<=.0031308?12.92*v:1.055*v**(1/2.4)-.055);
        const chroma=clean ? encode(Math.max(r,g,b))-encode(Math.min(r,g,b)) : 0;
        const neutral=clean && chroma<=(strong?20:12);
        const pixelMono=mono||neutral;
        const blend=clean ? clamp((chroma-(strong?20:12))/20,0,1) : 1;
        const colorGain=blend*blend*(3-2*blend);
        const neutralBias=clean && !pixelMono ? 1-colorGain : 0;
        if(hull && !neutral) {projectSixColor(r,g,b,hull,projected);[r,g,b]=projected;}
        if (gray || neutral) r = g = b = .2126 * r + .7152 * g + .0722 * b;
        if (gain) {
          if(pixelMono) {const v=.2126*current[e]+.7152*current[e+1]+.0722*current[e+2];r+=v;g+=v;b+=v;}
          else {r+=current[e];g+=current[e+1];b+=current[e+2];}
        }
        let best = inks[0], distance = Infinity;
        for (const ink of inks) {
          if(neutral && ink.id!==0 && ink.id!==1)continue;
          const dr = r - ink.lin[0], dg = g - ink.lin[1], db = b - ink.lin[2];
          const d = pixelMono ? (r - ink.luminance) ** 2 :
            (style === 'illustration' ? dr * dr + dg * dg + db * db :
              .3 * dr * dr + .5 * dg * dg + .2 * db * db);
          const score=d+(neutralBias && ink.id!==0 && ink.id!==1 ? .5*neutralBias*neutralBias : 0);
          if (score < distance) { best = ink; distance = score; }
        }
        ids[i] = best.id;
        preview[src] = best.rgb[0]; preview[src + 1] = best.rgb[1];
        preview[src + 2] = best.rgb[2]; preview[src + 3] = 255;
        if (gain) {
          const luminanceError=.2126*(r-best.lin[0])+.7152*(g-best.lin[1])+.0722*(b-best.lin[2]);
          for (let c = 0; c < 3; c++) {
            const value = c === 0 ? r : c === 1 ? g : b;
            let residual=value-(pixelMono ? best.luminance : best.lin[c]);
            if(clean && !pixelMono) residual=luminanceError+(residual-luminanceError)*colorGain;
            const error = clamp(residual, -1, 1) * (pixelMono ? 1 : gain);
            current[e + dir * 3 + c] += error * 7 / 16;
            next[e - dir * 3 + c] += error * 3 / 16;
            next[e + c] += error * 5 / 16;
            next[e + dir * 3 + c] += error / 16;
          }
        }
      }
      if (gain) { [current, next] = [next, current]; next.fill(0); }
      if ((y & 15) === 15) yield;
    }
    return { ids, preview };
  }
  function quantize(rgba, options) {
    const it = quantizeSteps(rgba, options);
    let step;
    do { step = it.next(); } while (!step.done);
    return step.value;
  }
  async function quantizeAsync(rgba, options) {
    const it = quantizeSteps(rgba, options);
    for (;;) {
      const step = it.next();
      if (step.done) return step.value;
      await new Promise(resolve => setTimeout(resolve, 0));
    }
  }
  function checksum(bytes) {
    let hash = 0x811c9dc5;
    for (const byte of bytes) hash = Math.imul(hash ^ byte, 0x01000193) >>> 0;
    return hash.toString(16).padStart(8, '0');
  }
  function makeFrame(ids, quality = 'best') {
    if (!Object.hasOwn(MODES, quality)) throw new Error('未知混色质量');
    if (!ids || ids.length !== WIDTH * HEIGHT) throw new Error('像素数量错误');
    const body = new Uint8Array(FRAME_BYTES), counts = new Uint32Array(7);
    let offset = 0;
    for (let x = WIDTH - 1; x >= 0; x--) {
      for (let y = 0; y < HEIGHT; y += 2) {
        const a = ids[sourceIndex(x, y)], b = ids[sourceIndex(x, y + 1)];
        if (!validInk(a) || !validInk(b)) throw new Error('六色帧包含无效色号');
        body[offset++] = (a << 4) | b; counts[a]++; counts[b]++;
      }
    }
    return { body, counts, format: FORMAT, quality, pass: 0, checksum: checksum(body) };
  }
  function previewIds(ids) {
    if (!(ids instanceof Uint8Array) || ids.length !== WIDTH * HEIGHT) throw new Error('像素数量错误');
    const lookup = [];
    for (const color of palette) lookup[color.id] = color.rgb;
    const preview = new Uint8ClampedArray(ids.length * 4);
    for (let i = 0; i < ids.length; i++) {
      const rgb = lookup[ids[i]];
      if (!rgb) throw new Error('六色预览包含无效色号');
      preview.set(rgb, i * 4); preview[i * 4 + 3] = 255;
    }
    return preview;
  }
  function composeRegion(base, next, region) {
    if (!(base instanceof Uint8Array) || !(next instanceof Uint8Array) || base.constructor !== next.constructor ||
        base.length !== WIDTH * HEIGHT || next.length !== base.length) throw new Error('区域更新需要完整基准画面');
    const {x, y, width, height} = region || {};
    if (![x,y,width,height].every(Number.isInteger) || x < 0 || y < 0 || width < 1 || height < 1 ||
        x + width > WIDTH || y + height > HEIGHT) throw new Error('更新区域超出屏幕');
    const result = base.slice();
    for (let row = y; row < y + height; row++) {
      const start = row * WIDTH + x;
      result.set(next.subarray(start, start + width), start);
    }
    return result;
  }
  async function verifyBaseline(baseline) {
    if (!baseline || !Number.isInteger(baseline.id) || !Number.isInteger(baseline.generation) || baseline.generation < 1) {
      throw new Error('请先完整发送一次画面，建立区域更新基准');
    }
    const job = await request('/api/job', {cache:'no-store'}, 4000);
    if (!job.ok || job.phase !== 'idle' || job.err !== 0 || job.id !== baseline.id ||
        job.display_generation !== baseline.generation || job.current_display_generation !== baseline.generation) {
      throw new Error('屏幕基准已变化，请重新发送完整画面');
    }
    return job;
  }
  // Read bodies before clearing the timer, so a stalled response also times out.
  async function request(url, options = {}, timeoutMs = 15000) {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), timeoutMs);
    try {
      const res = await fetch(url, { ...options, signal: controller.signal });
      const body = await res.text();
      if (!res.ok) throw new Error(body && !body.trimStart().startsWith('<') ? body : `HTTP ${res.status}`);
      return JSON.parse(body);
    } catch (err) {
      if (err.name === 'AbortError') throw Object.assign(new Error('设备响应超时，请检查连接及设备日志'),
        { name: 'TimeoutError', retryable: true });
      throw err;
    } finally { clearTimeout(timer); }
  }
  async function sendFrame(frame, onPhase = () => {}) {
    if (!frame || frame.format !== FORMAT || frame.pass !== 0 || !(frame.body instanceof Uint8Array) ||
        frame.body.length !== FRAME_BYTES || !Object.hasOwn(MODES, frame.quality) ||
        checksum(frame.body) !== frame.checksum || (frame.baseGeneration !== undefined &&
          (!Number.isInteger(frame.baseGeneration) || frame.baseGeneration < 1))) {
      throw new Error('帧格式或校验值错误');
    }
    onPhase('正在发送六色画面，请等待屏幕完成刷新…');
    const accepted = await request('/api/frame', {
      method: 'POST',
      headers: { 'Content-Type': 'application/octet-stream', 'X-Eink-Format': frame.format,
        'X-Eink-Pass': '0', 'X-Eink-Quality': frame.quality, 'X-Eink-Checksum': frame.checksum,
        ...(frame.baseGeneration ? {'X-Eink-Base-Generation': String(frame.baseGeneration)} : {}) },
      body: frame.body,
    }, 240000);
    if (!accepted.ok || !Number.isInteger(accepted.id) || accepted.id <= 0) throw new Error('设备协议不匹配，请更新固件并刷新网页');
    if (accepted.phase === 'error' || accepted.err) throw new Error(`屏幕刷新失败 (${accepted.err})`);
    let completed = accepted;
    if (accepted.phase !== 'idle') {
      onPhase('图片已接收，屏幕刷新中…');
      completed = await waitForJob(accepted.id, 180000);
    }
    if (completed.kind !== 'frame' || completed.sequence !== FORMAT || completed.err !== 0 ||
        completed.completed_mask !== 1 || completed.plane_fnv?.length !== 1 ||
        completed.plane_fnv[0] !== frame.checksum ||
        !Number.isInteger(completed.display_generation) || completed.display_generation < 1)
      throw new Error('六色刷新完成凭据不匹配，请更新固件并刷新网页');
    return completed;
  }
  async function waitForJob(id, timeoutMs) {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      const job = await request('/api/job', { cache: 'no-store' }, Math.min(15000, deadline - Date.now()));
      if (!job.ok || job.id !== id) throw new Error('刷新任务已变化，请重新发送');
      if (job.phase === 'error' || job.err) throw new Error(`屏幕刷新失败 (${job.err})`);
      if (job.phase === 'idle') return job;
      if (job.phase !== 'refresh' && job.phase !== 'receiving') throw new Error('设备返回未知刷新状态');
      await new Promise(resolve => setTimeout(resolve, 700));
    }
    throw new Error('屏幕刷新超时，请返回设备日志');
  }
  async function clearScreen() {
    const job = await request('/api/clear', {
      method: 'POST', body: '',
    });
    if (!job.ok || !Number.isInteger(job.id) || job.id <= 0 || job.kind !== 'clear-screen' ||
        job.sequence !== 'otp6-clear-v1')
      throw new Error('全局清屏请求协议不匹配');
    const completed = await waitForJob(job.id, 180000);
    if (completed.kind !== 'clear-screen' || completed.sequence !== 'otp6-clear-v1' ||
        completed.err !== 0 || completed.completed_mask !== 1 ||
        !Number.isInteger(completed.display_generation) || completed.display_generation < 1)
      throw new Error('全局清屏完成凭据不匹配');
    return completed;
  }
  return Object.freeze({ WIDTH, HEIGHT, FRAME_BYTES, FORMAT, COLORS,
    setPanelRotation, quantize, quantizeAsync, checksum, makeFrame,
    previewIds, composeRegion, verifyBaseline, sendFrame, clearScreen });
});
