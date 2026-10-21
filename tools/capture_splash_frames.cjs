// Render the original SVG CSS animation at exact 30 fps timestamps for the firmware.
// Usage: node tools/capture_splash_frames.cjs [svgPath] [outputDirectory]
const fs = require('node:fs');
const path = require('node:path');
const {pathToFileURL} = require('node:url');
let chromium;
try { ({chromium}=require('playwright')); }
catch { ({chromium}=require('C:/Users/Administrator/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright')); }
const root=path.resolve(__dirname,'..');
const source=path.resolve(process.argv[2]||path.join(root,'assets/njupt-splash-source.svg'));
const out=path.resolve(process.argv[3]||path.join(root,'splash-30fps'));
(async()=>{
  fs.mkdirSync(out,{recursive:true});
  const browser=await chromium.launch({headless:true,channel:'msedge'});
  try {
    const page=await browser.newPage({viewport:{width:720,height:720},deviceScaleFactor:1,reducedMotion:'no-preference'});
    await page.goto(pathToFileURL(source).href);
    const count=await page.evaluate(()=>{
      const animations=document.getAnimations();
      for(const animation of animations){animation.pause();animation.currentTime=0;}
      return animations.length;
    });
    if(count<4)throw Error('SVG animation is missing');
    // Preserve the 300 ms opening black hold and the final 500 ms logo hold.
    const times=[0,...Array.from({length:106},(_,i)=>Math.round((i+9)*1000/30))];
    const manifest=[];
    for(let i=0;i<times.length;i++){
      await page.evaluate(t=>{for(const a of document.getAnimations()){a.pause();a.currentTime=t;}},times[i]);
      const name=String(i).padStart(3,'0')+'.png';
      await page.screenshot({path:path.join(out,name),omitBackground:true});
      manifest.push({file:name,time_ms:times[i],duration_ms:i+1<times.length?times[i+1]-times[i]:500});
    }
    fs.writeFileSync(path.join(out,'timings.json'),JSON.stringify({fps:30,frames:manifest},null,2));
    const total=manifest.reduce((n,f)=>n+f.duration_ms,0);
    if(total!==4300 || times.at(-1)!==3800)throw Error('Unexpected animation timeline');
    console.log(JSON.stringify({frames:times.length,fps:30,total_ms:total,animations:count,out}));
  } finally { await browser.close(); }
})().catch(error=>{console.error(error);process.exit(1);});
