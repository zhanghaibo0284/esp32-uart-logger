#include "app_httpd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#ifndef DT_DIR
#define DT_DIR 4
#endif
#include <sys/stat.h>
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "dns_server.h"
#include "app_logger.h"
#include "app_settings.h"
#include "app_config.h"
#include "app_sd.h"
#include "app_time.h"
#include "app_wifi.h"
#include "app_bridge.h"

static const char *TAG = "httpd";
static httpd_handle_t s_server;
static const char WEB_PAGE[] = "<!DOCTYPE html>\r\n<html lang=\"zh\">\r\n<head>\r\n<meta charset=\"utf-8\">\r\n<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\r\n<title>串口记录仪</title>\r\n<style>\r\nbody{font:16px/1.45 -apple-system,BlinkMacSystemFont,\"Segoe UI\",sans-serif;margin:0;background:#0f1419;color:#e8eef5}\r\nheader,main{padding:12px}\r\nh1{font-size:20px;margin:0 0 6px}\r\nh2{font-size:16px;margin:0 0 8px}\r\n.card{background:#1b2430;border-radius:10px;padding:10px;margin:10px 0}\r\nlabel{display:block;margin:6px 0;color:#b7c3d1}\r\nselect,input,button{font-size:16px}\r\ninput{width:64px;padding:6px;border-radius:6px;border:0}\r\nbutton{background:#2f6fed;color:#fff;border:0;border-radius:8px;padding:12px 14px;margin:6px 6px 0 0}\r\n.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}\r\n.muted{color:#9aa8b8}\r\n.line{display:flex;align-items:center;gap:8px;margin:8px 0}\r\n.item{flex:1;display:block;color:#d7e6ff;text-decoration:none;background:#243044;border-radius:8px;padding:12px}\r\n.pick{width:22px;height:22px}\r\n#crumb a{color:#8ec5ff;margin-right:4px}\r\n.msg{min-height:1.2em;color:#ffd27a}\r\n#live{height:240px;overflow:auto;background:#0b0f14;padding:8px;white-space:pre-wrap;font:13px/1.35 ui-monospace,monospace}\r\n#cfg-file{width:auto;padding:0;background:none;border-radius:0}\r\nbutton.danger{background:#c0392b}\r\nbutton.small{padding:6px 10px;margin:0}\r\n.file-meta{color:#9aa8b8;font-size:13px;white-space:nowrap}\r\n#f-date,#f-kw{width:auto}\r\n#pager{gap:8px;margin-top:8px}\r\n#pager button{padding:8px 12px;margin:0}\r\n#pager .muted{font-size:13px}\r\n#files{height:420px;max-height:60vh;overflow-y:auto;background:#0b0f14;border-radius:8px;padding:0 6px;position:relative;-webkit-overflow-scrolling:touch}\r\n#files-ph{position:relative}\r\n#files .line{position:absolute;left:0;right:0;height:38px;margin:0;padding:0 2px;box-sizing:border-box}\r\n#files .item{padding:7px 10px;font-size:14px;line-height:22px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}\r\n#files .file-meta{font-size:12px}\r\n</style>\r\n</head>\r\n<body>\r\n<header>\r\n<h1>串口记录仪</h1>\r\n<div id=\"info\" class=\"muted\">正在读取...</div>\r\n</header>\r\n<main>\r\n<div id=\"ports\"></div>\r\n<div class=\"card\">\r\n<h2>串口透传</h2>\r\n<div class=\"muted\">WiFi TCP：用电脑 TCP 工具连接 192.168.4.1 对应端口即可双向透传。蓝牙为 BLE UART(NUS)，手机需用 BLE 调试工具(如 nRF Connect) 连接。</div>\r\n<div id=\"bridge\"></div>\r\n<div id=\"bridge-msg\" class=\"msg\"></div>\r\n</div>\r\n<div class=\"card\">\r\n<h2>实时数据</h2>\r\n<div class=\"row\">\r\n<label><input id=\"lv0\" class=\"pick\" type=\"checkbox\"> U0</label>\r\n<label><input id=\"lv1\" class=\"pick\" type=\"checkbox\" checked> J2</label>\r\n<label><input id=\"lv2\" class=\"pick\" type=\"checkbox\" checked> J3</label>\r\n<button type=\"button\" id=\"lv-clear\">清空</button>\r\n</div>\r\n<pre id=\"live\"></pre>\r\n</div>\r\n<div class=\"card\">\r\n<h2>校时</h2>\r\n<div class=\"row\">\r\n<label>年 <input id=\"yy\" inputmode=\"numeric\"></label>\r\n<label>月 <input id=\"mo\" inputmode=\"numeric\"></label>\r\n<label>日 <input id=\"dd\" inputmode=\"numeric\"></label>\r\n</div>\r\n<div class=\"row\">\r\n<label>时 <input id=\"hh\" inputmode=\"numeric\"></label>\r\n<label>分 <input id=\"mm\" inputmode=\"numeric\"></label>\r\n<label>秒 <input id=\"ss\" inputmode=\"numeric\"></label>\r\n</div>\r\n<button type=\"button\" id=\"btn-time\">校时</button>\r\n<button type=\"button\" id=\"btn-phone\">用手机时间</button>\r\n<div id=\"time-msg\" class=\"msg\"></div>\r\n</div>\r\n<div class=\"card\">\r\n<h2>数据记录</h2>\r\n<div class=\"muted\">可分批加载并浏览 TF 卡内全部日志记录（不再限制 100 条），支持按通道、日期、关键词筛选和时间/名称/大小排序；列表采用虚拟滚动，文件很多也不会卡顿。删除会立即从 TF 卡移除文件且不可恢复，正在写入的日志会自动跳过。</div>\r\n<div class=\"row\">\r\n<label>通道 <select id=\"f-channel\"><option value=\"all\">全部</option><option value=\"0\">U0</option><option value=\"1\">J2</option><option value=\"2\">J3</option></select></label>\r\n<label>日期 <input id=\"f-date\" type=\"date\"></label>\r\n<label>关键词 <input id=\"f-kw\" placeholder=\"文件名\"></label>\r\n<label>排序 <select id=\"f-sort\"><option value=\"time\">时间</option><option value=\"name\">名称</option><option value=\"size\">大小</option></select></label>\r\n<label>顺序 <select id=\"f-order\"><option value=\"desc\">降序</option><option value=\"asc\">升序</option></select></label>\r\n<label>每批 <select id=\"f-per\"><option value=\"200\">200</option><option value=\"500\" selected>500</option><option value=\"1000\">1000</option></select></label>\r\n<button type=\"button\" id=\"f-apply\">应用筛选</button>\r\n</div>\r\n<div class=\"row\">\r\n<button type=\"button\" id=\"btn-all\">全选已加载</button>\r\n<button type=\"button\" id=\"btn-zip\">下载选中</button>\r\n<button type=\"button\" id=\"btn-zipall\">下载全部</button>\r\n<button type=\"button\" id=\"btn-del\" class=\"danger\">删除选中</button>\r\n</div>\r\n<div id=\"crumb\" class=\"muted\"></div>\r\n<div class=\"row\">\r\n  <label>路径 <input id=\"f-path\" style=\"width:240px\" placeholder=\"/sdcard/...\"></label>\r\n  <button type=\"button\" id=\"f-goto\">跳转</button>\r\n  <button type=\"button\" id=\"f-root\">根目录</button>\r\n  <button type=\"button\" id=\"f-records\">全部记录视图</button>\r\n</div>\r\n<div id=\"files\"><div id=\"files-ph\"></div></div>\r\n<div class=\"row\" id=\"pager\"></div>\r\n<div id=\"file-msg\" class=\"msg\"></div>\r\n</div>\r\n<div class=\"card\">\r\n<h2>配置管理</h2>\r\n<div class=\"muted\">config.json 位于TF卡根目录：屏幕或本页修改参数都会自动写入，每次覆盖自动备份 config.json.bak。WiFi名称/密码与透传开关经文件加载后在下次启动生效。</div>\r\n<div class=\"row\">\r\n<button type=\"button\" id=\"cfg-download\">下载config.json</button>\r\n<button type=\"button\" id=\"cfg-export\">立即导出</button>\r\n<button type=\"button\" id=\"cfg-reload\">从卡重载</button>\r\n<button type=\"button\" id=\"cfg-restore\">恢复备份</button>\r\n</div>\r\n<div class=\"row\">\r\n<label>上传config.json <input id=\"cfg-file\" type=\"file\" accept=\".json,application/json\"></label>\r\n</div>\r\n<div id=\"cfg-msg\" class=\"msg\"></div>\r\n</div>\r\n</main>\r\n<script>\r\nconst names=[\"U0 板载\",\"J2\",\"J3\"];\r\nconst bauds=[115200,9600,19200,38400,57600,230400,460800,921600,4800,2400,1200];\r\nlet ports=[];\r\nfunction opts(list,val){\r\n  return list.map(function(v){\r\n    return '<option value=\"'+v+'\"'+(String(v)===String(val)?' selected':'')+'>'+v+'</option>';\r\n  }).join('');\r\n}\r\nfunction show(id,text){var el=document.getElementById(id); if(el) el.textContent=text||'';}\r\nfunction fillTime(text){\r\n  var m=/(\\d{4})-(\\d{2})-(\\d{2}) (\\d{2}):(\\d{2}):(\\d{2})/.exec(text||'');\r\n  if(!m) return;\r\n  document.getElementById('yy').value=m[1];\r\n  document.getElementById('mo').value=String(Number(m[2]));\r\n  document.getElementById('dd').value=String(Number(m[3]));\r\n  document.getElementById('hh').value=String(Number(m[4]));\r\n  document.getElementById('mm').value=String(Number(m[5]));\r\n  document.getElementById('ss').value=String(Number(m[6]));\r\n}\r\nfunction renderPorts(){\r\n  document.getElementById('ports').innerHTML=ports.map(function(p,i){\r\n    var st=p.open?('已打开 TX '+p.tx+' RX '+p.rx):(p.err?('失败 '+p.err):'已关闭');\r\n    return '<div class=\"card\" id=\"p'+i+'\"><b>'+names[i]+'</b> <span class=\"st\">'+st+'</span>'\r\n      +'<label>波特率 <select class=\"baud\">'+opts(bauds,p.baud)+'</select></label>'\r\n      +'<div class=\"row\"><label>数据位 <select class=\"bits\">'+opts([8,7,6,5],p.bits)+'</select></label>'\r\n      +'<label>停止位 <select class=\"stop\">'+opts(['1','1.5','2'],p.stop)+'</select></label>'\r\n      +'<label>校验 <select class=\"par\">'+opts(['N','E','O'],p.parity)+'</select></label></div>'\r\n      +'<button type=\"button\" data-act=\"toggle\" data-i=\"'+i+'\">'+(p.on?'关闭串口':'打开串口')+'</button>'\r\n      +'<button type=\"button\" data-act=\"apply\" data-i=\"'+i+'\">应用参数</button>'\r\n      +'<div class=\"msg\" id=\"m'+i+'\"></div></div>';\r\n  }).join('');\r\n}\r\nasync function loadStatus(fill){\r\n  try{\r\n    const s=await (await fetch('/api/status')).json();\r\n    show('info', s.time+'  '+(s.sd?('SD '+s.free_mb+' MB'):'无卡')+'  '+s.ssid);\r\n    if(fill){\r\n      ports=s.ports;\r\n      renderPorts();\r\n      fillTime(s.time);\r\n    }else{\r\n      s.ports.forEach(function(p,i){\r\n        ports[i].open=p.open; ports[i].tx=p.tx; ports[i].rx=p.rx; ports[i].err=p.err; ports[i].on=p.on;\r\n        var el=document.querySelector('#p'+i+' .st');\r\n        if(el) el.textContent=p.open?('已打开 TX '+p.tx+' RX '+p.rx):(p.err?('失败 '+p.err):'已关闭');\r\n        var btn=document.querySelector('#p'+i+' button[data-act=\"toggle\"]');\r\n        if(btn) btn.textContent=p.on?'关闭串口':'打开串口';\r\n      });\r\n    }\r\n  }catch(e){\r\n    show('info','读取失败');\r\n  }\r\n}\r\nfunction cardParams(i, on){\r\n  var card=document.getElementById('p'+i);\r\n  return new URLSearchParams({\r\n    index:String(i),\r\n    baud:card.querySelector('.baud').value,\r\n    bits:card.querySelector('.bits').value,\r\n    stop:card.querySelector('.stop').value,\r\n    parity:card.querySelector('.par').value,\r\n    on:on\r\n  });\r\n}\r\nasync function savePort(i, on){\r\n  show('m'+i,'请稍候');\r\n  try{\r\n    const r=await fetch('/api/port?'+cardParams(i,on).toString());\r\n    const j=await r.json();\r\n    show('m'+i, j.ok?(on==='1'?'已打开':'已关闭'):(j.err||'失败'));\r\n    if(j.ok){ ports[i].on=(on==='1'); }\r\n    await loadStatus(false);\r\n  }catch(e){\r\n    show('m'+i,'无响应');\r\n  }\r\n}\r\ndocument.getElementById('ports').addEventListener('click', function(ev){\r\n  var node=ev.target;\r\n  while(node && node.tagName!=='BUTTON') node=node.parentNode;\r\n  if(!node || !node.dataset) return;\r\n  var i=Number(node.dataset.i);\r\n  if(node.dataset.act==='toggle'){\r\n    savePort(i, ports[i] && ports[i].on ? '0' : '1');\r\n  }else if(node.dataset.act==='apply'){\r\n    savePort(i, (ports[i] && ports[i].on) ? '1' : '0');\r\n  }\r\n});\r\ndocument.getElementById('btn-time').addEventListener('click', async function(){\r\n  show('time-msg','请稍候');\r\n  var body=new URLSearchParams({\r\n    y:document.getElementById('yy').value,\r\n    mo:document.getElementById('mo').value,\r\n    d:document.getElementById('dd').value,\r\n    h:document.getElementById('hh').value,\r\n    mi:document.getElementById('mm').value,\r\n    s:document.getElementById('ss').value\r\n  });\r\n  try{\r\n    const r=await fetch('/api/time?'+body.toString());\r\n    const j=await r.json();\r\n    show('time-msg', j.ok?('校时成功 '+j.time):(j.err||'日期无效'));\r\n    if(j.ok) show('info', j.time);\r\n  }catch(e){\r\n    show('time-msg','无响应');\r\n  }\r\n});\r\nfunction safeDir(path){\r\n  if(!path || path.indexOf('/sdcard')!==0 || path.indexOf('..')>=0) return '/sdcard';\r\n  return path;\r\n}\r\nfunction renderCrumb(path){\r\n  var box=document.getElementById('crumb');\r\n  box.innerHTML='';\r\n  var acc='';\r\n  path.split('/').forEach(function(part){\r\n    if(!part) return;\r\n    acc+='/'+part;\r\n    if(box.childNodes.length){\r\n      var sep=document.createElement('span');\r\n      sep.textContent=' / ';\r\n      box.appendChild(sep);\r\n    }\r\n    var a=document.createElement('a');\r\n    a.textContent=part;\r\n    a.href='/?dir='+encodeURIComponent(acc);\r\n    a.setAttribute('data-dir','1');\r\n    a.setAttribute('data-path',acc);\r\n    box.appendChild(a);\r\n  });\r\n  // 同步路径输入框，便于用户编辑后跳转到任意目录。\r\n  var pin=document.getElementById('f-path');\r\n  if(pin) pin.value=path;\r\n}\r\n// ---- 数据记录：全量分批加载 + 虚拟滚动（保留筛选/排序/选择/导出/删除） ----\r\nvar FROW=38, FOVER=8;                 // 行高与视口上下额外渲染的行数\r\nvar fstate={path:'/sdcard',sort:'time',order:'desc',channel:'all',date:'',kw:'',batch:500,records:false};\r\nvar fdata=[];        // 已加载条目（服务端已按当前条件排序）\r\nvar ftotal=0;        // 筛选后的总条数\r\nvar floading=false;  // 是否有请求在进行\r\nvar fdone=false;     // 是否已加载完全部\r\nvar ftrunc=false;    // 服务端索引是否达到容量上限\r\nvar fsel={};         // 选中集合：相对名 -> true（跨滚动窗口保持）\r\nvar fseq=0;          // 加载代次：丢弃过期响应，避免快速切换筛选时串数据\r\nvar fwinFirst=-1, fwinLast=-1;   // 当前已渲染的窗口区间，未变化时跳过重绘\r\nfunction fmtSize(n){\r\n  if(n>=1048576) return (n/1048576).toFixed(1)+' MB';\r\n  if(n>=1024) return (n/1024).toFixed(1)+' KB';\r\n  return n+' B';\r\n}\r\nfunction fmtTime(sec){\r\n  if(!sec || sec<946684800) return '';\r\n  var d=new Date(sec*1000);\r\n  function p(v){return v<10?('0'+v):(''+v);}\r\n  return d.getFullYear()+'-'+p(d.getMonth()+1)+'-'+p(d.getDate())+' '+p(d.getHours())+':'+p(d.getMinutes());\r\n}\r\nfunction fileFull(rel){\r\n  return (fstate.path==='/'?'':fstate.path)+'/'+rel;\r\n}\r\nfunction fileDirName(rel){\r\n  var full=fileFull(rel);\r\n  var i=full.lastIndexOf('/');\r\n  return {dir: full.slice(0,i)||'/sdcard', name: full.slice(i+1)};\r\n}\r\n// 只渲染滚动窗口内的行：DOM 节点始终只有几十个，\r\n// 即使已加载上万条也不会造成页面卡顿或内存暴涨。\r\nfunction renderFiles(force){\r\n  var box=document.getElementById('files');\r\n  var ph=document.getElementById('files-ph');\r\n  if(!fdata.length){\r\n    fwinFirst=-1; fwinLast=-1;\r\n    ph.style.height='40px';\r\n    ph.innerHTML='';\r\n    var empty=document.createElement('div');\r\n    empty.className='muted';\r\n    empty.style.padding='10px';\r\n    empty.textContent=floading?'正在读取...':'暂无记录';\r\n    ph.appendChild(empty);\r\n    return;\r\n  }\r\n  ph.style.height=(fdata.length*FROW)+'px';\r\n  var first=Math.max(0, Math.floor(box.scrollTop/FROW)-FOVER);\r\n  var last=Math.min(fdata.length, Math.ceil((box.scrollTop+box.clientHeight)/FROW)+FOVER);\r\n  if(!force && first===fwinFirst && last===fwinLast) return;\r\n  fwinFirst=first; fwinLast=last;\r\n  var frag=document.createDocumentFragment();\r\n  for(var i=first;i<last;i++){\r\n    var it=fdata[i];\r\n    var line=document.createElement('div');\r\n    line.className='line';\r\n    line.style.top=(i*FROW)+'px';\r\n    if(it.dir){\r\n      var a=document.createElement('a');\r\n      a.className='item';\r\n      a.textContent=it.name+'/';\r\n      a.href='/?dir='+encodeURIComponent(fileFull(it.name));\r\n      a.setAttribute('data-dir','1');\r\n      a.setAttribute('data-path', fileFull(it.name));\r\n      line.appendChild(a);\r\n      var dl=document.createElement('button');\r\n      dl.type='button';\r\n      dl.className='small';\r\n      dl.textContent='下载';\r\n      dl.setAttribute('data-dl-dir', it.name);\r\n      line.appendChild(dl);\r\n    }else{\r\n      var ck=document.createElement('input');\r\n      ck.type='checkbox';\r\n      ck.className='pick';\r\n      ck.checked=!!fsel[it.name];\r\n      ck.setAttribute('data-name', it.name);\r\n      line.appendChild(ck);\r\n      var link=document.createElement('a');\r\n      link.className='item';\r\n      link.textContent=it.name;\r\n      link.href='/dl?path='+encodeURIComponent(fileFull(it.name));\r\n      line.appendChild(link);\r\n      var meta=document.createElement('span');\r\n      meta.className='file-meta';\r\n      meta.textContent=fmtSize(it.size||0)+(it.mtime?('  '+fmtTime(it.mtime)):'');\r\n      line.appendChild(meta);\r\n      var del=document.createElement('button');\r\n      del.type='button';\r\n      del.className='danger small';\r\n      del.textContent='删除';\r\n      del.setAttribute('data-del', it.name);\r\n      line.appendChild(del);\r\n    }\r\n    frag.appendChild(line);\r\n  }\r\n  ph.innerHTML='';\r\n  ph.appendChild(frag);\r\n}\r\n// 状态栏：已加载/总数 + 加载全部 + 刷新\r\nfunction renderStatus(){\r\n  var box=document.getElementById('pager');\r\n  box.innerHTML='';\r\n  var info=document.createElement('span');\r\n  info.className='muted';\r\n  var txt='已加载 '+fdata.length+' / 共 '+ftotal+' 条';\r\n  if(floading) txt+='（读取中…）';\r\n  if(ftrunc) txt+='；已达索引上限，请用筛选缩小范围';\r\n  info.textContent=txt;\r\n  box.appendChild(info);\r\n  if(!fdone && fdata.length){\r\n    var all=document.createElement('button');\r\n    all.type='button';\r\n    all.textContent='加载全部';\r\n    all.onclick=loadAll;\r\n    box.appendChild(all);\r\n  }\r\n  var rel=document.createElement('button');\r\n  rel.type='button';\r\n  rel.textContent='刷新';\r\n  rel.onclick=function(){ loadFiles(fstate.path); };\r\n  box.appendChild(rel);\r\n}\r\nfunction loadedFileNames(){\r\n  var out=[];\r\n  for(var i=0;i<fdata.length;i++){ if(!fdata[i].dir) out.push(fdata[i].name); }\r\n  return out;\r\n}\r\nfunction renderSelButtons(){\r\n  var names=loadedFileNames();\r\n  var selCount=Object.keys(fsel).length;\r\n  var all=names.length>0 && names.every(function(n){ return fsel[n]; });\r\n  var a=document.getElementById('btn-all');\r\n  if(a) a.textContent=all?'取消全选':'全选已加载';\r\n  var d=document.getElementById('btn-del');\r\n  if(d) d.textContent=selCount?('删除选中('+selCount+')'):'删除选中';\r\n}\r\nfunction fileQuery(offset){\r\n  var q={\r\n    path:fstate.path, offset:String(offset), per_page:String(fstate.batch),\r\n    sort:fstate.sort, order:fstate.order, channel:fstate.channel,\r\n    date:fstate.date, kw:fstate.kw\r\n  };\r\n  if(fstate.records) q.rec='1';\r\n  return new URLSearchParams(q).toString();\r\n}\r\n// 拉取一批记录。带 8 秒超时：设备忙/网络差时主动中断，\r\n// 避免请求永远挂起把界面卡在“读取中…”。\r\nasync function fetchBatch(offset){\r\n  var ctl=null, timer=0;\r\n  try{\r\n    ctl=new AbortController();\r\n    timer=setTimeout(function(){ ctl.abort(); }, 8000);\r\n    var r=await fetch('/api/files?'+fileQuery(offset), {signal:ctl.signal, cache:'no-store'});\r\n    if(!r.ok) throw new Error('HTTP '+r.status);\r\n    return await r.json();\r\n  }finally{\r\n    if(timer) clearTimeout(timer);\r\n  }\r\n}\r\n// 重新加载第一批：换目录 / 改筛选 / 刷新 / 删除后调用。\r\n// keepScroll=true 时尽量恢复原滚动位置（单条删除后保留浏览位置）。\r\nasync function loadFiles(path, keepScroll){\r\n  if(path!==undefined) fstate.path=safeDir(path);\r\n  var box=document.getElementById('files');\r\n  var keep=keepScroll?box.scrollTop:0;\r\n  fseq++;\r\n  var my=fseq;\r\n  fdata=[]; ftotal=0; fdone=false; ftrunc=false; floading=true; fsel={};\r\n  fwinFirst=-1; fwinLast=-1;\r\n  box.scrollTop=0;\r\n  show('file-msg','');\r\n  renderFiles(true); renderStatus(); renderSelButtons();\r\n  try{\r\n    var s=await fetchBatch(0);\r\n    if(my!==fseq) return;\r\n    if(s.err){\r\n      show('file-msg', s.err);\r\n      fdone=true;\r\n    }else{\r\n      fstate.path=safeDir(s.path||fstate.path);\r\n      ftotal=s.total||0;\r\n      ftrunc=!!s.truncated;\r\n      fdata=s.items||[];\r\n      fdone=fdata.length>=ftotal;\r\n      renderCrumb(fstate.path);\r\n      box.scrollTop=keep;\r\n    }\r\n  }catch(e){\r\n    if(my!==fseq) return;\r\n    show('file-msg', e && e.name==='AbortError'\r\n      ? '读取超时（设备无响应），请检查连接后点“刷新”重试'\r\n      : '记录读取失败，请点“刷新”重试');\r\n    fdone=true;\r\n  }\r\n  floading=false;\r\n  renderFiles(true); renderStatus(); renderSelButtons();\r\n}\r\n// 按 offset 追加下一批；滚动到底部时自动触发\r\nasync function loadMore(){\r\n  if(floading || fdone) return;\r\n  var my=fseq;\r\n  floading=true;\r\n  renderStatus();\r\n  try{\r\n    var s=await fetchBatch(fdata.length);\r\n    if(my!==fseq) return;\r\n    var items=s.items||[];\r\n    for(var i=0;i<items.length;i++) fdata.push(items[i]);\r\n    ftotal=s.total||ftotal;\r\n    ftrunc=!!s.truncated;\r\n    fdone=!items.length || fdata.length>=ftotal;\r\n  }catch(e){\r\n    if(my!==fseq) return;\r\n    fdone=true;\r\n    show('file-msg', e && e.name==='AbortError'\r\n      ? '分批加载超时，请点“刷新”重试'\r\n      : '分批加载失败，请点“刷新”重试');\r\n  }\r\n  floading=false;\r\n  renderFiles(true); renderStatus(); renderSelButtons();\r\n}\r\n// 「加载全部」：循环拉取所有批次直到取完（期间状态栏显示进度）\r\nasync function loadAll(){\r\n  if(floading) return;\r\n  show('file-msg','正在加载全部记录…');\r\n  while(!fdone && !floading){\r\n    await loadMore();\r\n  }\r\n  show('file-msg', fdone?('已加载全部 '+fdata.length+' 条'):'');\r\n}\r\nfunction applyFilters(){\r\n  fstate.channel=document.getElementById('f-channel').value;\r\n  fstate.date=document.getElementById('f-date').value||'';\r\n  fstate.kw=document.getElementById('f-kw').value.trim();\r\n  fstate.sort=document.getElementById('f-sort').value;\r\n  fstate.order=document.getElementById('f-order').value;\r\n  fstate.batch=Number(document.getElementById('f-per').value)||500;\r\n  loadFiles();\r\n}\r\ndocument.getElementById('f-apply').addEventListener('click', applyFilters);\r\ndocument.getElementById('f-kw').addEventListener('keydown', function(ev){ if(ev.key==='Enter') applyFilters(); });\r\ndocument.getElementById('f-sort').addEventListener('change', applyFilters);\r\ndocument.getElementById('f-order').addEventListener('change', applyFilters);\r\ndocument.getElementById('f-per').addEventListener('change', applyFilters);\r\ndocument.getElementById('f-channel').addEventListener('change', applyFilters);\r\ndocument.getElementById('f-date').addEventListener('change', applyFilters);\r\nfunction onDirClick(ev){\r\n  var node=ev.target;\r\n  while(node && node.tagName!=='A') node=node.parentNode;\r\n  if(!node || node.getAttribute('data-dir')!=='1') return;\r\n  ev.preventDefault();\r\n  loadFiles(node.getAttribute('data-path'));\r\n}\r\ndocument.getElementById('crumb').addEventListener('click', onDirClick);\r\ndocument.getElementById('files').addEventListener('click', onDirClick);\r\n// 滚动时按帧重绘可视行；接近底部时自动追加下一批\r\nvar fRAF=0;\r\ndocument.getElementById('files').addEventListener('scroll', function(){\r\n  if(!fRAF){\r\n    fRAF=requestAnimationFrame(function(){ fRAF=0; renderFiles(false); });\r\n  }\r\n  var box=document.getElementById('files');\r\n  if(box.scrollTop+box.clientHeight>=box.scrollHeight-160) loadMore();\r\n});\r\nwindow.addEventListener('resize', function(){ renderFiles(true); });\r\n// 全选/取消全选：作用范围是“已加载”的全部文件（不受虚拟滚动可视范围影响）\r\ndocument.getElementById('btn-all').addEventListener('click', function(){\r\n  var names=loadedFileNames();\r\n  var all=names.length>0 && names.every(function(n){ return fsel[n]; });\r\n  if(all){\r\n    fsel={};\r\n  }else{\r\n    names.forEach(function(n){ fsel[n]=true; });\r\n  }\r\n  renderFiles(true);\r\n  renderSelButtons();\r\n});\r\n// 勾选变化写入选中集合，跨滚动窗口保持\r\ndocument.getElementById('files').addEventListener('change', function(ev){\r\n  var t=ev.target;\r\n  if(!t || t.className!=='pick') return;\r\n  var nm=t.getAttribute('data-name');\r\n  if(!nm) return;\r\n  if(t.checked){ fsel[nm]=true; } else { delete fsel[nm]; }\r\n  renderSelButtons();\r\n});\r\nfunction selectedNames(){\r\n  return Object.keys(fsel);\r\n}\r\n// 从 Content-Disposition 里解析出 filename（优先 filename*=UTF-8''）\r\nfunction parseFileName(cd){\r\n  if(!cd) return 'archive.zip';\r\n  var m=cd.match(/filename\\*\\s*=\\s*UTF-8''([^;]+)/i);\r\n  if(m) try{ return decodeURIComponent(m[1]); }catch(e){}\r\n  m=cd.match(/filename\\s*=\\s*\"([^\"]+)\"/i);\r\n  if(m) return m[1];\r\n  m=cd.match(/filename\\s*=\\s*([^;]+)/i);\r\n  if(m) return m[1].trim();\r\n  return 'archive.zip';\r\n}\r\n// ZIP 下载：流式读取响应体，边收边在状态栏显示已收字节数，\r\n// 既支持大文件夹打包不会因长时间无反馈导致界面“假死”，\r\n// 又能在服务端返回 404/413 时给出明确错误提示。\r\nasync function downloadZip(url){\r\n  show('file-msg','正在打包，请稍候…');\r\n  try{\r\n    var r=await fetch(url);\r\n    if(!r.ok){\r\n      var t=await r.text();\r\n      if(r.status===413) show('file-msg','文件太多无法一次打包，请按通道/日期/关键词筛选后再试');\r\n      else show('file-msg','打包失败：'+(t||r.status));\r\n      return;\r\n    }\r\n    var name=parseFileName(r.headers.get('Content-Disposition'));\r\n    if(!r.body){\r\n      var blob0=await r.blob();\r\n      saveBlob(blob0, name);\r\n      show('file-msg','打包完成：'+fmtSize(blob0.size));\r\n      return;\r\n    }\r\n    var reader=r.body.getReader();\r\n    var chunks=[];\r\n    var got=0;\r\n    for(;;){\r\n      var res=await reader.read();\r\n      if(res.done) break;\r\n      chunks.push(res.value);\r\n      got+=res.value.length;\r\n      show('file-msg','正在下载：已收 '+fmtSize(got)+' …');\r\n    }\r\n    var blob=new Blob(chunks, {type:'application/zip'});\r\n    saveBlob(blob, name);\r\n    show('file-msg','打包完成：'+fmtSize(got)+' → '+name);\r\n  }catch(e){\r\n    show('file-msg', e && e.name==='AbortError'\r\n      ? '下载超时，请检查连接后重试' : '打包请求无响应');\r\n  }\r\n}\r\nfunction saveBlob(blob, name){\r\n  var a=document.createElement('a');\r\n  a.href=URL.createObjectURL(blob);\r\n  a.download=name||'logs.zip';\r\n  document.body.appendChild(a);\r\n  a.click();\r\n  document.body.removeChild(a);\r\n  setTimeout(function(){ URL.revokeObjectURL(a.href); }, 30000);\r\n}\r\n// 下载某个子文件夹：把该目录下所有层级的文件打包成 ZIP。\r\nfunction downloadFolder(rel){\r\n  var full=fileFull(rel);\r\n  show('file-msg','正在打包文件夹：'+rel);\r\n  downloadZip('/api/zip?dir='+encodeURIComponent(full)+'&all=1');\r\n}\r\nfunction zipParams(){\r\n  return 'channel='+encodeURIComponent(fstate.channel)+\r\n         '&date='+encodeURIComponent(fstate.date)+\r\n         '&kw='+encodeURIComponent(fstate.kw);\r\n}\r\ndocument.getElementById('btn-zip').addEventListener('click', function(){\r\n  var names=selectedNames();\r\n  if(!names.length){ show('file-msg','请选择要下载的记录'); return; }\r\n  var q='/api/zip?dir='+encodeURIComponent(fstate.path)+'&names='+encodeURIComponent(names.join('|'));\r\n  if(q.length>3200){ show('file-msg','选中文件过多，请分批下载或改用“下载全部”'); return; }\r\n  downloadZip(q);\r\n});\r\n// “下载全部”= 当前目录下所有层级的文件全部打包（含子目录）\r\ndocument.getElementById('btn-zipall').addEventListener('click', function(){\r\n  downloadZip('/api/zip?dir='+encodeURIComponent(fstate.path)+'&all=1&'+zipParams());\r\n});\r\n// 路径导航\r\ndocument.getElementById('f-goto').addEventListener('click', function(){\r\n  var v=document.getElementById('f-path').value.trim();\r\n  if(!v) v='/sdcard';\r\n  loadFiles(v);\r\n});\r\ndocument.getElementById('f-root').addEventListener('click', function(){\r\n  loadFiles('/sdcard');\r\n});\r\ndocument.getElementById('f-records').addEventListener('click', function(){\r\n  fstate.records=!fstate.records;\r\n  document.getElementById('f-records').textContent=fstate.records?'返回目录视图':'全部记录视图';\r\n  loadFiles(fstate.path);\r\n});\r\n// 文件夹“下载”按钮（事件委托，因虚拟滚动只渲染可视行）\r\ndocument.getElementById('files').addEventListener('click', function(e){\r\n  var t=e.target;\r\n  if(t && t.getAttribute && t.getAttribute('data-dl-dir')){\r\n    e.preventDefault();\r\n    downloadFolder(t.getAttribute('data-dl-dir'));\r\n  }\r\n});\r\n// 批量删除：按目录分组、每批 40 条串行提交，最后汇总成功/失败原因\r\nasync function deleteSelected(){\r\n  var names=selectedNames();\r\n  if(!names.length){ show('file-msg','请先勾选要删除的记录'); return; }\r\n  if(!window.confirm('确定删除选中的 '+names.length+' 条记录？\\n删除后不可恢复。')) return;\r\n  show('file-msg','正在删除...');\r\n  // 记录模式下列表可能跨 UART0/1/2 多个目录，按目录分组后逐批提交。\r\n  var groups={};\r\n  names.forEach(function(rel){\r\n    var p=fileDirName(rel);\r\n    (groups[p.dir]=groups[p.dir]||[]).push(p.name);\r\n  });\r\n  var ok=0, bad=0, reasons=[];\r\n  try{\r\n    var dirs=Object.keys(groups);\r\n    for(var d=0; d<dirs.length; d++){\r\n      var list=groups[dirs[d]];\r\n      for(var i=0;i<list.length;i+=40){\r\n        var part=list.slice(i,i+40);\r\n        var s=await apiGet('/api/delete?dir='+encodeURIComponent(dirs[d])+'&names='+encodeURIComponent(part.join('|')));\r\n        ok+=(s.deleted||0);\r\n        bad+=(s.failed||0);\r\n        (s.items||[]).forEach(function(it){ if(!it.ok) reasons.push(it.name+'：'+(it.err||'失败')); });\r\n      }\r\n    }\r\n    var msg='删除完成：成功 '+ok+' 条，失败 '+bad+' 条';\r\n    if(reasons.length) msg+='（'+reasons.slice(0,3).join('；')+(reasons.length>3?' 等':'')+'）';\r\n    show('file-msg', msg);\r\n  }catch(e){\r\n    show('file-msg','删除请求无响应');\r\n  }\r\n  // 无论成败都刷新：已删除项消失，失败项继续保留在列表中\r\n  await loadFiles(fstate.path);\r\n}\r\ndocument.getElementById('btn-del').addEventListener('click', deleteSelected);\r\n// 单条删除：二次确认 → 调接口 → 提示结果 → 保留滚动位置刷新\r\nasync function deleteOne(rel){\r\n  var p=fileDirName(rel);\r\n  if(!window.confirm('确定删除这条记录？\\n'+rel+'\\n删除后不可恢复。')) return;\r\n  show('file-msg','正在删除...');\r\n  try{\r\n    var s=await apiGet('/api/delete?dir='+encodeURIComponent(p.dir)+'&name='+encodeURIComponent(p.name));\r\n    if(s.deleted===1) show('file-msg','删除成功：'+rel);\r\n    else{\r\n      var why='失败';\r\n      (s.items||[]).forEach(function(it){ if(!it.ok) why=it.err||'失败'; });\r\n      show('file-msg','删除失败：'+why);\r\n    }\r\n  }catch(e){\r\n    show('file-msg','删除请求无响应');\r\n  }\r\n  await loadFiles(fstate.path, true);\r\n}\r\ndocument.getElementById('files').addEventListener('click', function(ev){\r\n  var node=ev.target;\r\n  while(node && node.tagName!=='BUTTON') node=node.parentNode;\r\n  if(!node || !node.getAttribute) return;\r\n  var rel=node.getAttribute('data-del');\r\n  if(rel){ deleteOne(rel); }\r\n});\r\ndocument.getElementById('btn-phone').addEventListener('click', async function(){\r\n  var d=new Date();\r\n  document.getElementById('yy').value=d.getFullYear();\r\n  document.getElementById('mo').value=d.getMonth()+1;\r\n  document.getElementById('dd').value=d.getDate();\r\n  document.getElementById('hh').value=d.getHours();\r\n  document.getElementById('mm').value=d.getMinutes();\r\n  document.getElementById('ss').value=d.getSeconds();\r\n  show('time-msg','请稍候');\r\n  var body=new URLSearchParams({\r\n    y:d.getFullYear(), mo:d.getMonth()+1, d:d.getDate(),\r\n    h:d.getHours(), mi:d.getMinutes(), s:d.getSeconds()\r\n  });\r\n  try{\r\n    const r=await fetch('/api/time?'+body.toString());\r\n    const j=await r.json();\r\n    show('time-msg', j.ok?('校时成功 '+j.time):(j.err||'日期无效'));\r\n    if(j.ok) show('info', j.time);\r\n  }catch(e){\r\n    show('time-msg','无响应');\r\n  }\r\n});\r\n\r\n\r\nlet liveCursor=[0,0,0];\r\ndocument.getElementById('lv-clear').addEventListener('click', function(){\r\n  document.getElementById('live').textContent='';\r\n  liveCursor=[0,0,0];\r\n});\r\nasync function pollLive(){\r\n  var q=[];\r\n  for(var i=0;i<3;i++){\r\n    var box=document.getElementById('lv'+i);\r\n    if(box && box.checked) q.push(i+'='+liveCursor[i]);\r\n  }\r\n  if(!q.length) return;\r\n  try{\r\n    const s=await (await fetch('/api/live?'+q.join('&'))).json();\r\n    var view=document.getElementById('live');\r\n    var stick=view.scrollTop+view.clientHeight>=view.scrollHeight-24;\r\n    (s.ports||[]).forEach(function(p){\r\n      liveCursor[p.i]=p.next;\r\n      if(p.text) view.textContent+=p.text;\r\n    });\r\n    if(view.textContent.length>24000) view.textContent=view.textContent.slice(-14000);\r\n    if(stick) view.scrollTop=view.scrollHeight;\r\n  }catch(e){}\r\n}\r\n\r\nloadStatus(true);\r\nvar startDir='/sdcard';\r\ntry{ var qs=new URLSearchParams(location.search); startDir=safeDir(qs.get('dir')||'/sdcard'); }catch(e){}\r\nloadFiles(startDir);\r\nsetInterval(function(){loadStatus(false);},2000);\r\nsetInterval(pollLive,500);\r\n\r\nfunction apiGet(url){ return fetch(url).then(function(r){return r.json();}); }\r\nfunction renderBridge(){\r\n  apiGet('/api/bridge').then(function(b){\r\n    var labels=['U0','J2','J3'];\r\n    var rows=b.tcp.map(function(t,i){\r\n      return '<div class=\"line\"><b style=\"width:34px\">'+labels[i]+'</b>'\r\n        +'<label>TCP端口 <input class=\"bport\" style=\"width:90px\" value=\"'+t.port+'\"></label>'\r\n        +'<button type=\"button\" class=\"btcp\" data-i=\"'+i+'\">'+(t.on?('停止(客户端'+t.clients+')'):'开启TCP')+'</button>'\r\n        +'<span class=\"muted\">收'+t.rx+' 发'+t.tx+'</span></div>';\r\n    }).join('');\r\n    var ble=b.ble;\r\n    rows+='<div class=\"line\"><b style=\"width:34px\">BLE</b>'\r\n      +'<label>绑定 <select id=\"ble-port\"><option value=\"0\">U0</option><option value=\"1\"'+(ble.port===1?' selected':'')+'>J2</option><option value=\"2\"'+(ble.port===2?' selected':'')+'>J3</option></select></label>'\r\n      +'<label>名称 <input id=\"ble-name\" style=\"width:120px\" value=\"'+ble.name+'\"></label>'\r\n      +'<button type=\"button\" id=\"b-ble\">'+(ble.on?(ble.connected?'已连接,停止':'广播中,停止'):'开启BLE')+'</button>'\r\n      +'<span class=\"muted\">收'+ble.rx+' 发'+ble.tx+'</span></div>';\r\n    document.getElementById('bridge').innerHTML=rows;\r\n    Array.prototype.forEach.call(document.querySelectorAll('.btcp'),function(btn){\r\n      btn.onclick=function(){\r\n        var i=btn.getAttribute('data-i');\r\n        var on=btn.textContent.indexOf('开启')===0;\r\n        var port=btn.parentElement.querySelector('.bport').value;\r\n        apiGet('/api/bridge_tcp?index='+i+'&on='+(on?1:0)+'&port='+port).then(function(r){\r\n          show('bridge-msg',r.ok?'已更新':('失败 '+(r.err||''))); renderBridge();\r\n        });\r\n      };\r\n    });\r\n    document.getElementById('b-ble').onclick=function(){\r\n      var on=document.getElementById('b-ble').textContent.indexOf('开启')===0;\r\n      var p=document.getElementById('ble-port').value;\r\n      var nm=encodeURIComponent(document.getElementById('ble-name').value);\r\n      apiGet('/api/bridge_ble?index='+p+'&on='+(on?1:0)+'&name='+nm).then(function(r){\r\n        show('bridge-msg',r.ok?'已更新':('失败 '+(r.err||''))); renderBridge();\r\n      });\r\n    };\r\n  });\r\n}\r\n\r\nsetInterval(renderBridge, 3000);\r\n\r\n// --- configuration management ---\r\nfunction cfgMsg(text){ show('cfg-msg', text); }\r\ndocument.getElementById('cfg-download').onclick = function(){\r\n  window.location.href='/dl?path='+encodeURIComponent('/sdcard/config.json');\r\n};\r\nfunction cfgOp(op, done){\r\n  cfgMsg('请稍候');\r\n  apiGet('/api/config?op='+op).then(function(r){\r\n    cfgMsg(r.ok ? (r.msg||'完成') : (r.err||'失败'));\r\n    if(r.ok) { loadStatus(true); }\r\n    if(done) done(r);\r\n  }).catch(function(){ cfgMsg('无响应'); });\r\n}\r\ndocument.getElementById('cfg-export').onclick = function(){ cfgOp('export'); };\r\ndocument.getElementById('cfg-reload').onclick = function(){ cfgOp('reload'); };\r\ndocument.getElementById('cfg-restore').onclick = function(){ cfgOp('restore'); };\r\ndocument.getElementById('cfg-file').addEventListener('change', function(ev){\r\n  var f = ev.target.files[0];\r\n  if(!f) return;\r\n  if(f.size > 8000){ cfgMsg('文件需小于8KB'); return; }\r\n  cfgMsg('请稍候');\r\n  f.text().then(function(t){\r\n    return fetch('/api/config_upload', {method:'POST', body:t});\r\n  }).then(function(r){ return r.json(); }).then(function(r){\r\n    cfgMsg(r.ok ? '上传并加载成功' : (r.err||'失败'));\r\n    if(r.ok){ loadStatus(true); }\r\n  }).catch(function(){ cfgMsg('上传失败'); });\r\n});\r\n</script>\r\n</body>\r\n</html>\r\n";

static int bits_of(uart_word_length_t bits)
{
    if (bits == UART_DATA_7_BITS) return 7;
    if (bits == UART_DATA_6_BITS) return 6;
    if (bits == UART_DATA_5_BITS) return 5;
    return 8;
}

static const char *stop_of(uart_stop_bits_t stop)
{
    if (stop == UART_STOP_BITS_1_5) return "1.5";
    if (stop == UART_STOP_BITS_2) return "2";
    return "1";
}

static const char *parity_of(uart_parity_t parity)
{
    if (parity == UART_PARITY_EVEN) return "E";
    if (parity == UART_PARITY_ODD) return "O";
    return "N";
}

static bool parse_bits(const char *text, uart_word_length_t *out)
{
    if (!strcmp(text, "8")) { *out = UART_DATA_8_BITS; return true; }
    if (!strcmp(text, "7")) { *out = UART_DATA_7_BITS; return true; }
    if (!strcmp(text, "6")) { *out = UART_DATA_6_BITS; return true; }
    if (!strcmp(text, "5")) { *out = UART_DATA_5_BITS; return true; }
    return false;
}

static bool parse_stop(const char *text, uart_stop_bits_t *out)
{
    if (!strcmp(text, "1")) { *out = UART_STOP_BITS_1; return true; }
    if (!strcmp(text, "1.5")) { *out = UART_STOP_BITS_1_5; return true; }
    if (!strcmp(text, "2")) { *out = UART_STOP_BITS_2; return true; }
    return false;
}

static bool parse_parity(const char *text, uart_parity_t *out)
{
    if (!strcmp(text, "N")) { *out = UART_PARITY_DISABLE; return true; }
    if (!strcmp(text, "E")) { *out = UART_PARITY_EVEN; return true; }
    if (!strcmp(text, "O")) { *out = UART_PARITY_ODD; return true; }
    return false;
}

/* 校验请求路径必须位于 /sdcard 之下，且不含 ".."。
 * 返回 true=允许访问。用于所有文件类接口，防止目录穿越读到系统其它路径。
 */
static bool path_ok(const char *path)
{
    if (!path || strncmp(path, "/sdcard", 7) != 0) {
        return false;
    }
    if (path[7] != '\0' && path[7] != '/') {
        return false;
    }
    if (strstr(path, "..")) {
        return false;
    }
    return true;
}

/* 把字符串转义成可安全放入 JSON 字符串的形式：
 *   " 和 \ → 前置反斜杠；控制字符（<0x20）直接丢弃；其余原样输出。
 * 参数 dst/dst_len：输出缓冲，超长自动截断并保证以 '\0' 结尾。
 * 说明：中文 UTF-8 多字节原样保留（网页按 UTF-8 解析），不转 \uXXXX。
 */
static void json_escape(const char *src, char *dst, size_t dst_len)
{
    size_t n = 0;
    for (size_t i = 0; src[i] && n + 2 < dst_len; i++) {
        unsigned char ch = (unsigned char)src[i];
        if (ch == '"' || ch == '\\') {
            if (n + 3 >= dst_len) break;
            dst[n++] = '\\';
            dst[n++] = (char)ch;
        } else if (ch < 0x20) {
            continue;
        } else {
            dst[n++] = (char)ch;
        }
    }
    dst[n] = '\0';
}

static bool req_value(httpd_req_t *req, const char *key, char *out, size_t out_len);

/* /api/bridge：返回 TCP（每路开/关、端口、客户端数、收发/丢弃计数）与
 * BLE（开关、连接状态、绑定通道、收发计数、设备名）的实时状态。
 * 输出为单条 JSON（<=1200 字节静态缓冲），失败时仍返回截断后的有效 JSON。
 */
static esp_err_t bridge_status_get(httpd_req_t *req)
{
    bridge_info_t info;
    app_bridge_get(&info);
    char body[1200];
    int n = snprintf(body, sizeof(body),
                     "{\"tcp\":[");
    for (int i = 0; i < APP_PORT_COUNT && n > 0; i++) {
        n += snprintf(body + n, sizeof(body) - (size_t)n,
                      "%s{\"on\":%s,\"port\":%u,\"clients\":%d,\"rx\":%llu,\"tx\":%llu,"
                      "\"updrop\":%llu,\"downdrop\":%llu}",
                      i ? "," : "",
                      info.tcp_on[i] ? "true" : "false",
                      info.tcp_port[i], info.tcp_clients[i],
                      (unsigned long long)info.tcp_rx[i],
                      (unsigned long long)info.tcp_tx[i],
                      (unsigned long long)info.up_drop[i],
                      (unsigned long long)info.down_drop[i]);
    }
    n += snprintf(body + n, sizeof(body) - (size_t)n,
                  "],\"ble\":{\"on\":%s,\"connected\":%s,\"port\":%d,\"rx\":%llu,\"tx\":%llu,\"name\":\"%s\"}}",
                  info.ble_on ? "true" : "false",
                  info.ble_connected ? "true" : "false",
                  info.ble_port,
                  (unsigned long long)info.ble_rx,
                  (unsigned long long)info.ble_tx,
                  info.ble_name);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t bridge_tcp_post(httpd_req_t *req)
{
    char index_text[8] = {0};
    char on_text[8] = {0};
    char port_text[8] = {0};
    if (!req_value(req, "index", index_text, sizeof(index_text)) ||
        !req_value(req, "on", on_text, sizeof(on_text)) ||
        !req_value(req, "port", port_text, sizeof(port_text))) {
        httpd_resp_sendstr(req, "{\"ok\":0,\"err\":\"missing\"}");
        return ESP_OK;
    }
    int index = atoi(index_text);
    int port = atoi(port_text);
    int rc = app_bridge_tcp_set(index, strcmp(on_text, "1") == 0, (uint16_t)port);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, rc == 0 ? "{\"ok\":1}" : "{\"ok\":0,\"err\":\"fail\"}");
}

static esp_err_t bridge_ble_post(httpd_req_t *req)
{
    char index_text[8] = {0};
    char on_text[8] = {0};
    char name_text[BRIDGE_NAME_LEN + 1] = {0};
    if (!req_value(req, "index", index_text, sizeof(index_text)) ||
        !req_value(req, "on", on_text, sizeof(on_text))) {
        httpd_resp_sendstr(req, "{\"ok\":0,\"err\":\"missing\"}");
        return ESP_OK;
    }
    req_value(req, "name", name_text, sizeof(name_text));
    int index = atoi(index_text);
    int rc = app_bridge_ble_set(strcmp(on_text, "1") == 0, index, name_text);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, rc == 0 ? "{\"ok\":1}" : "{\"ok\":0,\"err\":\"fail\"}");
}

/* 返回内嵌网页（WEB_PAGE）。字符串是本文件顶部由 web_index.html 转义而来，
 * 修改网页后必须重新同步，否则线上仍是旧页面。 */
static esp_err_t send_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, WEB_PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t root_get(httpd_req_t *req)
{
    return send_page(req);
}

/* /api/status：页面初始化与轮询的核心接口。
 * 返回：当前时间、SD 是否挂载/剩余 MB、SSID/IP，以及三路串口的
 *       打开状态(on/open)、收发字节数、错误码与参数（波特率/位/停止/校验）。
 * 实现：分别从 settings/logger/wifi 取快照后拼装 JSON；容量不足时会
 *       提前停止追加端口，仍保证 JSON 合法闭合。
 */
static esp_err_t status_get(httpd_req_t *req)
{
    app_settings_t cfg;
    app_view_t view;
    app_wifi_info_t wifi;
    char now[24];
    app_settings_get(&cfg);
    app_logger_get_view(&view);
    app_wifi_get(&wifi);
    app_time_format(now, sizeof(now));

    char body[1400];
    int n = snprintf(body, sizeof(body),
                     "{\"time\":\"%s\",\"sd\":%s,\"free_mb\":%lu,\"ssid\":\"%s\",\"ip\":\"%s\",\"ports\":[",
                     now, view.sd_mounted ? "true" : "false",
                     (unsigned long)view.sd_free_mb, wifi.ssid, wifi.ip);
    for (int i = 0; i < APP_PORT_COUNT && n > 0 && n < (int)sizeof(body) - 80; i++) {
        n += snprintf(body + n, sizeof(body) - (size_t)n,
                      "%s{\"open\":%s,\"on\":%s,\"tx\":%llu,\"rx\":%llu,\"err\":%d,\"baud\":%lu,\"bits\":%d,\"stop\":\"%s\",\"parity\":\"%s\"}",
                      i ? "," : "",
                      view.port[i].open ? "true" : "false",
                      cfg.port[i].enabled ? "true" : "false",
                      (unsigned long long)view.port[i].tx_bytes,
                      (unsigned long long)view.port[i].rx_bytes,
                      view.port[i].error,
                      (unsigned long)cfg.port[i].baud,
                      bits_of(cfg.port[i].data_bits),
                      stop_of(cfg.port[i].stop_bits),
                      parity_of(cfg.port[i].parity));
    }
    snprintf(body + n, sizeof(body) - (size_t)n, "]}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

/* 把单个十六进制字符转成 0..15；非法字符返回 -1。 */
static int hex_nibble(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* 就地 URL 解码：%XX → 单字节，"+" → 空格。
 * 参数 text：待解码字符串（原地修改，输出一定不长于输入）。
 * 非法 %XX（缺字符或非十六进制）按原样保留，不会越界读取。
 */
static void url_decode(char *text)
{
    char *read = text;
    char *write = text;
    if (!text) {
        return;
    }
    while (*read) {
        if (*read == '%' ) {
            int hi = hex_nibble(read[1]);
            int lo = hex_nibble(read[2]);
            if (hi >= 0 && lo >= 0) {
                *write++ = (char)((hi << 4) | lo);
                read += 3;
                continue;
            }
        } else if (*read == '+') {
            *write++ = ' ';
            read++;
            continue;
        }
        *write++ = *read++;
    }
    *write = '\0';
}

/* 读取 URL 查询串中的指定键并做 URL 解码。
 * 参数：key 键名；out/out_len 输出缓冲。
 * 返回 true=键存在且已解码；false=无查询串或键不存在。
 * 说明：查询串总长限制 512 字节（覆盖 path+筛选+分页等组合参数）。
 */
static bool req_value(httpd_req_t *req, const char *key, char *out, size_t out_len)
{
    // 查询串缓冲需要容纳 path/kw/date 等组合参数，512 字节足够单次请求使用。
    char query[512] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    if (httpd_query_key_value(query, key, out, out_len) != ESP_OK) {
        return false;
    }
    url_decode(out);
    return true;
}

/* ---- 数据记录（SD 卡日志文件）浏览与删除 -------------------------------
 *
 * Web 页「数据记录」展示的是 TF 卡上的串口日志文件（*.txt）。服务端负责
 * 扫描、过滤、排序与分页/分批，前端按 offset 增量拉取并虚拟滚动渲染，
 * 因此可以完整浏览卡内全部记录，而不会把上万条目录项一次性塞进网页。
 */

#define FILE_SCAN_MAX 12000     // 单次索引最多条目（PSRAM 约 1.3MB）；超出置 truncated
#define FILE_NAME_MAX 96        // 相对路径名缓冲（"UART1/20261027_235959.txt"）
#define FILE_PATH_MAX 512       // 绝对路径缓冲
#define SCAN_PATH_MAX 192       // 递归扫描用的短路径缓冲（/sdcard/日期/UARTx/文件名 最长约 130）
#define FILE_BATCH_MAX 1000     // 单次请求最多返回条数（前端按批加载）
#define FILE_INDEX_TTL_US (4 * 1000 * 1000LL)   // 目录索引缓存有效期 4 秒
#define ZIP_MAX 8000            // 单次 ZIP 最多打包条数（PSRAM 约 0.9MB）

typedef struct {
    char name[FILE_NAME_MAX];   // 相对浏览基目录的名称
    uint32_t size;              // 文件大小（字节），目录为 0
    uint32_t mtime;             // 修改时间（epoch 秒），用于“最新优先”
    bool is_dir;                // true=目录
} file_ent_t;

typedef struct {
    int channel;                // -1=全部通道；0/1/2 只匹配 /UARTx/
    char date[9];               // "YYYYMMDD"，空串表示不按日期筛选
    char kw[48];                // 小写关键词，空串表示不按关键词筛选
} file_filter_t;

// 排序参数放在静态变量中供 qsort 比较函数使用。esp_http_server 为每个连接
// 创建独立任务，处理器可能并发执行，因此所有访问都包在 s_file_lock 内。
static int s_sort_field;        // 0=时间 1=名称 2=大小
static int s_sort_desc;         // 1=降序 0=升序

// 目录索引缓存：首次请求扫描一次 FAT，此后 4 秒内（或删除/重挂载前）复用，
// 避免翻页/分批加载时反复遍历上千个目录项。缓冲区固定 FILE_SCAN_MAX 容量，
// 只在第一次分配，重建时原地覆盖，减少 PSRAM 碎片。
static file_ent_t *s_index_ents;    // PSRAM 中的条目数组
static int s_index_count;           // 当前缓存的有效条目数
static bool s_index_valid;          // 缓存是否可用
static bool s_index_truncated;      // 扫描是否因容量上限被截断
static uint32_t s_index_gen;        // 缓存对应的文件系统代次
static int64_t s_index_us;          // 缓存建立时刻（微秒）
static char s_index_key[320];       // 缓存键：路径 + 筛选 + 排序
static uint32_t s_file_gen = 1;     // 文件系统代次，删除/重挂载后自增使缓存失效
static SemaphoreHandle_t s_file_lock;   // 保护索引与排序比较参数

/* 获取/释放文件列表索引锁；首次调用时创建互斥量。 */
static void files_lock(void)
{
    if (!s_file_lock) {
        s_file_lock = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_file_lock, portMAX_DELAY);
}

static void files_unlock(void)
{
    if (s_file_lock) {
        xSemaphoreGive(s_file_lock);
    }
}

/* 使目录索引缓存失效（删除文件/重新挂载后调用）。
 * 只自增代次并标记无效，缓冲区保留复用，避免频繁申请/释放 PSRAM。
 */
static void file_index_invalidate(void)
{
    files_lock();
    s_file_gen++;
    s_index_valid = false;
    files_unlock();
}

// 判断是否以 ".txt" 结尾：记录模式只收集日志文件，避免把 config.json 混入。
static bool has_txt_suffix(const char *name)
{
    size_t n = name ? strlen(name) : 0;
    return n > 4 && strcmp(name + n - 4, ".txt") == 0;
}

// 大小写无关的子串匹配；needle 必须事先转成小写。
static bool ci_contains(const char *hay, const char *needle)
{
    if (!needle[0]) {
        return true;
    }
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] && (char)tolower((unsigned char)p[i]) == needle[i]) {
            i++;
        }
        if (i == nl) {
            return true;
        }
    }
    return false;
}

// 拼接“相对路径”：prefix 为空时直接拷贝 name，否则拼成 prefix/name；超长截断。
// 用显式循环而非 snprintf，避免 GCC 对最大文件名长度做格式截断告警。
static void join_rel(char *dst, size_t n, const char *prefix, const char *name)
{
    size_t p = 0;
    if (prefix && prefix[0]) {
        while (prefix[p] && p + 1 < n) {
            dst[p] = prefix[p];
            p++;
        }
        if (p + 1 < n) {
            dst[p++] = '/';
        }
    }
    size_t j = 0;
    while (name[j] && p + 1 < n) {
        dst[p++] = name[j++];
    }
    dst[p] = '\0';
}

// 向结果数组追加一个条目；rel 为递归时相对基目录前缀。
static void add_ent(file_ent_t *ents, int *count, const char *rel, const char *name,
                    uint32_t size, uint32_t mtime, bool is_dir)
{
    if (*count >= FILE_SCAN_MAX) {
        s_index_truncated = true;       // 达到上限：列表不完整，前端会提示
        return;
    }
    file_ent_t *e = &ents[*count];
    join_rel(e->name, sizeof(e->name), rel, name);
    e->size = size;
    e->mtime = mtime;
    e->is_dir = is_dir;
    (*count)++;
}

/* 计算公历日期距 1970-01-01 的天数（Howard Hinnant 算法）。
 * 仅用于把文件名/FAT 时间转成 epoch 秒做排序，不依赖设备时区设置。
 */
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

/* 从日志文件名（YYYYmmdd_HHMMSS.txt）直接推导时间戳，避免逐文件 stat。
 * 参数 rel：相对名（可能带 "UART1/" 前缀）；返回 epoch 秒，格式不符返回 0。
 */
static uint32_t name_epoch(const char *rel)
{
    const char *base = strrchr(rel, '/');
    base = base ? base + 1 : rel;
    for (int i = 0; i < 8; i++) {
        if (!isdigit((unsigned char)base[i])) {
            return 0;
        }
    }
    if (base[8] != '_') {
        return 0;
    }
    for (int i = 9; i < 15; i++) {
        if (!isdigit((unsigned char)base[i])) {
            return 0;
        }
    }
    int y = (base[0] - '0') * 1000 + (base[1] - '0') * 100 + (base[2] - '0') * 10 + (base[3] - '0');
    int mo = (base[4] - '0') * 10 + (base[5] - '0');
    int d = (base[6] - '0') * 10 + (base[7] - '0');
    int h = (base[9] - '0') * 10 + (base[10] - '0');
    int mi = (base[11] - '0') * 10 + (base[12] - '0');
    int s = (base[13] - '0') * 10 + (base[14] - '0');
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 59) {
        return 0;
    }
    return (uint32_t)(days_from_civil(y, (unsigned)mo, (unsigned)d) * 86400 + h * 3600 + mi * 60 + s);
}

/* 把 FILINFO 的 FAT 日期/时间转成 epoch 秒；未设置时间返回 0。
 * FAT 时间精度为 2 秒，仅用于非日志文件的排序与显示。
 */
static uint32_t fil_epoch(const FILINFO *fi)
{
    if (!fi->fdate) {
        return 0;
    }
    int y = 1980 + ((fi->fdate >> 9) & 0x7F);
    unsigned mo = (fi->fdate >> 5) & 0x0F;
    unsigned d = fi->fdate & 0x1F;
    unsigned h = (fi->ftime >> 11) & 0x1F;
    unsigned mi = (fi->ftime >> 5) & 0x3F;
    unsigned s = (fi->ftime & 0x1F) * 2u;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59) {
        return 0;
    }
    return (uint32_t)(days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s);
}

/* 把 VFS 绝对路径 "/sdcard/xxx" 映射为 FatFs 卷内路径 "0:/xxx"。
 * 直接调用 FatFs 的 f_readdir 可一次取回 fsize/fdate/ftime；
 * 若改用 POSIX stat，FAT 上每个文件都要从头扫描目录，文件多时退化为 O(n²)。
 */
static bool to_fat_path(const char *vfs, char *out, size_t n)
{
    size_t ml = strlen(APP_SD_MOUNT);
    if (strncmp(vfs, APP_SD_MOUNT, ml) != 0) {
        return false;
    }
    const char *rest = vfs + ml;
    if (*rest == '\0') {
        rest = "/";
    } else if (*rest != '/') {
        return false;                   // 拒绝 "/sdcardX" 之类前缀误匹配
    }
    snprintf(out, n, "0:%s", rest);
    return true;
}

/* 递归扫描目录并收集匹配项（单次遍历，不做 stat）。
 * 参数：
 *   dir    当前绝对目录；rel 相对基目录前缀（用于拼接展示名）
 *   depth  剩余递归层数（记录模式下根目录传 1，只向下展开一层）
 *   records true=记录模式：只收 .txt、不展示目录；false=目录模式：展示目录和全部文件
 *   filter 通道/日期/关键词筛选条件
 * 返回：通过 ents/count 输出；目录打不开或容量用尽时静默结束。
 */
static void scan_dir(const char *dir, const char *rel, int depth, bool records,
                     file_ent_t *ents, int *count, const file_filter_t *filter)
{
    // 递归函数：路径缓冲改用短缓冲放在栈上，避免多层递归 + 大缓冲把 httpd 任务栈打爆。
    char fpath[SCAN_PATH_MAX];
    if (!to_fat_path(dir, fpath, sizeof(fpath))) {
        return;
    }
    FF_DIR dp;
    if (f_opendir(&dp, fpath) != FR_OK) {
        return;
    }
    FILINFO fi;
    memset(&fi, 0, sizeof(fi));
    while (*count < FILE_SCAN_MAX && f_readdir(&dp, &fi) == FR_OK && fi.fname[0]) {
        const char *nm = fi.fname;
        if (nm[0] == '.') {
            continue;                   // 跳过 "."、".." 与隐藏文件
        }
        bool is_dir = (fi.fattrib & AM_DIR) != 0;
        if (is_dir) {
            if (records && depth > 0) {
                // 记录模式下递归展开到日志文件层。新结构为 /sdcard/YYYY-MM-DD/UARTx/*.txt，
                // 根目录 records 模式 depth=2，才能越过“日期目录 + 通道子目录”两层拿到 *.txt。
                char full[SCAN_PATH_MAX];
                char child[FILE_NAME_MAX];
                join_rel(child, sizeof(child), rel, nm);
                join_rel(full, sizeof(full), dir, nm);
                scan_dir(full, child, depth - 1, records, ents, count, filter);
            } else if (!records) {
                add_ent(ents, count, rel, nm, 0, fil_epoch(&fi), true);
            }
            continue;
        }
        if (records && !has_txt_suffix(nm)) {
            continue;                   // 记录模式忽略非日志文件
        }
        char full[SCAN_PATH_MAX];
        join_rel(full, sizeof(full), dir, nm);
        if (filter) {
            if (filter->channel >= 0) {
                char tag[16];
                // 通道号已限定在 0..APP_PORT_COUNT-1，& 7 同时让编译器确定输出长度上限
                snprintf(tag, sizeof(tag), "/UART%d/", filter->channel & 7);
                if (!strstr(full, tag)) {
                    continue;
                }
            }
            if (filter->date[0] && strncmp(nm, filter->date, 8) != 0) {
                continue;               // 文件名以 YYYYmmdd 开头
            }
            if (filter->kw[0] && !ci_contains(full, filter->kw)) {
                continue;
            }
        }
        uint32_t mt = name_epoch(nm);
        if (!mt) {
            mt = fil_epoch(&fi);
        }
        add_ent(ents, count, rel, nm, (uint32_t)fi.fsize, mt, false);
    }
    f_closedir(&dp);
}

// qsort 比较：目录始终排在文件之前；同类型按选定字段排序，字段相同用名称兜底。
static int ent_cmp(const void *pa, const void *pb)
{
    const file_ent_t *a = (const file_ent_t *)pa;
    const file_ent_t *b = (const file_ent_t *)pb;
    if (a->is_dir != b->is_dir) {
        return a->is_dir ? -1 : 1;
    }
    int rc = 0;
    if (s_sort_field == 0) {
        if (a->mtime != b->mtime) {
            rc = (a->mtime < b->mtime) ? -1 : 1;
        } else {
            rc = strcmp(a->name, b->name);
        }
    } else if (s_sort_field == 1) {
        rc = strcmp(a->name, b->name);
    } else {
        if (a->size != b->size) {
            rc = (a->size < b->size) ? -1 : 1;
        } else {
            rc = strcmp(a->name, b->name);
        }
    }
    return s_sort_desc ? -rc : rc;
}

/* 建立/复用目录索引（调用方必须已持有 s_file_lock）。
 * 参数：path 浏览目录；records 记录模式；filter 筛选条件；sort_field/desc 排序。
 * 返回：true=索引可用（含空目录）；false=PSRAM 分配失败。
 * 关键点：
 *   1. 路径+筛选+排序完全一致且未超过 TTL 时直接复用，翻页/分批加载不再扫卡；
 *   2. 索引缓冲区只分配一次（FILE_SCAN_MAX 条目约 1.3MB，必须放 PSRAM）；
 *   3. 扫描与 qsort 都在锁内完成，避免并发请求互相踩踏排序参数。
 */
static bool build_index(const char *path, bool records, const file_filter_t *filter,
                        int sort_field, bool desc)
{
    char key[320];
    snprintf(key, sizeof(key), "%s|%d|%d|%s|%s|%d|%d",
             path, records ? 1 : 0, filter->channel, filter->date, filter->kw,
             sort_field, desc ? 1 : 0);
    int64_t now = esp_timer_get_time();
    if (s_index_valid && s_index_gen == s_file_gen && !strcmp(key, s_index_key) &&
        now - s_index_us < FILE_INDEX_TTL_US) {
        return true;
    }
    if (!s_index_ents) {
        s_index_ents = heap_caps_malloc(FILE_SCAN_MAX * sizeof(file_ent_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_index_ents) {
            return false;           // 大数组必须放 PSRAM，绝不允许挤占内部 RAM
        }
    }
    s_index_valid = false;
    s_index_count = 0;
    s_index_truncated = false;
    s_sort_field = sort_field;
    s_sort_desc = desc ? 1 : 0;
    app_fs_lock();
    scan_dir(path, "", records ? 2 : 0, records, s_index_ents, &s_index_count, filter);
    app_fs_unlock();
    if (s_index_count > 1) {
        qsort(s_index_ents, (size_t)s_index_count, sizeof(file_ent_t), ent_cmp);
    }
    snprintf(s_index_key, sizeof(s_index_key), "%s", key);
    s_index_gen = s_file_gen;
    s_index_us = now;
    s_index_valid = true;
    return true;
}

/* 解析列表/打包共用的筛选参数（channel / date / kw）。
 * 参数：filter 输出结构，内部先清零；channel 默认 -1（全部）。
 * 说明：date 只保留 8 位数字（接受 YYYY-MM-DD 或 YYYYMMDD），非法时清空；
 *       kw 统一转小写后再做大小写无关匹配；不合法通道号按“全部”处理。
 */
static void parse_filter(httpd_req_t *req, file_filter_t *filter)
{
    char text[64];
    memset(filter, 0, sizeof(*filter));
    filter->channel = -1;
    if (req_value(req, "channel", text, sizeof(text)) && text[0]) {
        if (strcmp(text, "all") != 0) {
            const char *digits = strstr(text, "UART");
            digits = digits ? digits + 4 : text;
            int idx = atoi(digits);
            if (idx >= 0 && idx < APP_PORT_COUNT) {
                filter->channel = idx;
            }
        }
    }
    if (req_value(req, "date", text, sizeof(text)) && text[0]) {
        int d = 0;
        for (const char *p = text; *p && d < 8; p++) {
            if (*p >= '0' && *p <= '9') {
                filter->date[d++] = *p;
            }
        }
        filter->date[d] = '\0';
        if (d != 8) {
            filter->date[0] = '\0';      // 非法日期忽略，避免误筛空列表
        }
    }
    if (req_value(req, "kw", text, sizeof(text))) {
        size_t n = strlen(text);
        if (n >= sizeof(filter->kw)) {
            n = sizeof(filter->kw) - 1;
        }
        for (size_t i = 0; i < n; i++) {
            filter->kw[i] = (char)tolower((unsigned char)text[i]);
        }
        filter->kw[n] = '\0';
    }
}

/* /api/files
 * 参数：
 *   path      浏览目录（默认 /sdcard）
 *   offset    起始偏移（从 0 开始；给出时优先于 page，便于前端分批加载）
 *   page      页码，从 1 开始（默认 1）
 *   per_page  每批条数（默认 100，范围 1..1000）
 *   sort      time|name|size（默认 time）
 *   order     desc|asc（默认 desc，即最新优先）
 *   channel   all|0|1|2（也接受 UART0..UART2）
 *   date      YYYY-MM-DD 或 YYYYMMDD（按文件名日期前缀筛选）
 *   kw        文件名/路径关键词（大小写无关）
 *   rec       1=强制记录模式（递归收集各级日志）；默认 0，仅列当前目录直接子项。
 * 返回 JSON：{path,page,per_page,offset,total,pages,has_more,truncated,
 *             sort,order,items[{name,dir,size,mtime}]}
 * 说明：items[].name 是相对 path 的名称；记录模式下形如 "UART1/20261027_....txt"。
 * 异常：PSRAM 不足返回 err；目录打开失败返回空列表。
 * 性能：全量索引最多 FILE_SCAN_MAX 条，命中缓存时响应只需拷贝当前页。
 */
static esp_err_t files_get(httpd_req_t *req)
{
    char path[160] = "/sdcard";
    char value[160] = {0};
    char text[48] = {0};

    if (req_value(req, "path", value, sizeof(value)) && path_ok(value)) {
        snprintf(path, sizeof(path), "%s", value);
    }
    int page = 1;
    if (req_value(req, "page", text, sizeof(text))) {
        page = atoi(text);
        if (page < 1) {
            page = 1;
        }
    }
    int offset = -1;                     // -1 表示未使用 offset 参数
    if (req_value(req, "offset", text, sizeof(text))) {
        offset = atoi(text);
        if (offset < 0) {
            offset = 0;
        }
    }
    int per = 100;                       // 需求：默认展示最新 100 条
    if (req_value(req, "per_page", text, sizeof(text))) {
        per = atoi(text);
    }
    if (per < 1) {
        per = 100;
    } else if (per > FILE_BATCH_MAX) {
        per = FILE_BATCH_MAX;
    }
    int sort_field = 0;
    if (req_value(req, "sort", text, sizeof(text))) {
        if (!strcmp(text, "name")) {
            sort_field = 1;
        } else if (!strcmp(text, "size")) {
            sort_field = 2;
        }
    }
    bool desc = true;                    // 默认降序：时间最新的在前
    if (req_value(req, "order", text, sizeof(text)) && !strcmp(text, "asc")) {
        desc = false;
    }

    file_filter_t filter;
    parse_filter(req, &filter);
    // 默认只列出当前目录的直接子项（根目录直接显示日期文件夹与配置文件），
    // 避免一进入页面就遍历整张卡导致卡顿；需要“全部通道记录”视图时前端显式传 rec=1。
    bool records = false;
    if (req_value(req, "rec", text, sizeof(text))) {
        records = strcmp(text, "1") == 0;
    }

    // 索引构建/分页取数都在锁内完成；响应体所需的页数据先拷贝出来，
    // 释放锁后再慢速发送，避免慢客户端阻塞其它文件请求。
    files_lock();
    bool indexed = build_index(path, records, &filter, sort_field, desc);
    int total = s_index_count;
    int pages = total ? (total + per - 1) / per : 0;
    int start;
    if (offset >= 0) {
        start = offset;
        page = start / per + 1;          // 保持 page 字段与 offset 自洽
    } else {
        if (pages > 0 && page > pages) {
            page = pages;                // 删除后页码越界时自动回退到最后一页
        }
        start = (page - 1) * per;
    }
    if (start > total) {
        start = total;
    }
    int end = start + per;
    if (end > total) {
        end = total;
    }
    int take = end - start;
    file_ent_t *page_ents = NULL;
    if (take > 0) {
        page_ents = heap_caps_malloc((size_t)take * sizeof(file_ent_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (page_ents) {
            memcpy(page_ents, s_index_ents + start, (size_t)take * sizeof(file_ent_t));
        }
    }
    bool truncated = s_index_truncated;
    files_unlock();

    if (!indexed || (take > 0 && !page_ents)) {
        free(page_ents);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
        return ESP_FAIL;
    }

    char escaped[320];
    json_escape(path, escaped, sizeof(escaped));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char head[460];
    int hn = snprintf(head, sizeof(head),
                      "{\"path\":\"%s\",\"page\":%d,\"per_page\":%d,\"offset\":%d,"
                      "\"total\":%d,\"pages\":%d,\"has_more\":%d,\"truncated\":%d,"
                      "\"sort\":\"%s\",\"order\":\"%s\",\"items\":[",
                      escaped, page, per, start, total, pages, end < total ? 1 : 0,
                      truncated ? 1 : 0,
                      sort_field == 1 ? "name" : (sort_field == 2 ? "size" : "time"),
                      desc ? "desc" : "asc");
    esp_err_t err = httpd_resp_send_chunk(req, head, hn);
    for (int i = 0; i < take && err == ESP_OK; i++) {
        char nm[FILE_NAME_MAX * 2];
        json_escape(page_ents[i].name, nm, sizeof(nm));
        char item[400];
        int ilen = snprintf(item, sizeof(item),
                            "%s{\"name\":\"%s\",\"dir\":%s,\"size\":%lu,\"mtime\":%lu}",
                            i > 0 ? "," : "", nm,
                            page_ents[i].is_dir ? "true" : "false",
                            (unsigned long)page_ents[i].size,
                            (unsigned long)page_ents[i].mtime);
        err = httpd_resp_send_chunk(req, item, ilen);
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, "]}", 2);
    }
    if (err == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);
    }
    free(page_ents);
    return err;
}

// 判断目标文件是否为某个串口正在写入的活动日志；活动文件严禁删除。
static bool is_active_log_file(const char *full)
{
    app_view_t view;
    app_logger_get_view(&view);
    for (int i = 0; i < APP_PORT_COUNT; i++) {
        if (!view.port[i].file_name[0]) {
            continue;
        }
        char active[128];
        // file_name 现存储相对路径 YYYY-MM-DD/UARTx/filename.txt
        snprintf(active, sizeof(active), APP_SD_MOUNT "/%s", view.port[i].file_name);
        if (!strcmp(active, full)) {
            return true;
        }
    }
    return false;
}

/* /api/delete 删除数据记录（仅允许 *.txt）
 * 参数：
 *   dir    文件所在目录（默认 /sdcard，必须在 /sdcard 下且不含 ".."）
 *   name   单个文件名（与 names 二选一）
 *   names  多个文件名，用 "|" 连接（与 name 二选一）
 * 返回 JSON：{ok,deleted,failed,items[{name,ok,err?}]}
 * 异常：文件名非法/路径逃逸/文件不存在/目标为目录/正在写入时，对应条目
 *       failed 并带 err 说明；删除动作在 app_fs_lock 保护下执行。
 */
static esp_err_t delete_get(httpd_req_t *req)
{
    char *query = malloc(2600);
    char *names = malloc(2100);
    if (!query || !names) {
        free(query);
        free(names);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
        return ESP_FAIL;
    }
    char dir[192] = "/sdcard";
    names[0] = '\0';
    if (httpd_req_get_url_query_str(req, query, 2600) == ESP_OK) {
        char value[192];
        if (httpd_query_key_value(query, "dir", value, sizeof(value)) == ESP_OK) {
            url_decode(value);
            if (path_ok(value)) {
                snprintf(dir, sizeof(dir), "%s", value);
            }
        }
        char one[128];
        if (httpd_query_key_value(query, "name", one, sizeof(one)) == ESP_OK && one[0]) {
            url_decode(one);
            snprintf(names, 2100, "%s", one);
        } else if (httpd_query_key_value(query, "names", names, 2100) == ESP_OK) {
            url_decode(names);
        }
    }
    free(query);
    if (!names[0]) {
        free(names);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":0,\"err\":\"missing name\"}");
    }

    char *out = heap_caps_malloc(24576, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) {
        out = malloc(12288);
    }
    if (!out) {
        free(names);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
        return ESP_FAIL;
    }
    int cap = 24000;
    int n = snprintf(out, cap, "{\"items\":[");
    int deleted = 0;
    int failed = 0;
    int total = 0;
    bool first = true;
    char *saveptr = NULL;
    for (char *tok = strtok_r(names, "|", &saveptr); tok && total < 120;
         tok = strtok_r(NULL, "|", &saveptr)) {
        // 去掉 URL 解码后可能出现的首尾空白
        while (*tok == ' ') {
            tok++;
        }
        size_t tl = strlen(tok);
        while (tl && (tok[tl - 1] == '\r' || tok[tl - 1] == '\n' || tok[tl - 1] == ' ')) {
            tok[--tl] = '\0';
        }
        total++;
        const char *reason = NULL;
        char full[FILE_PATH_MAX];
        if (!tok[0] || strchr(tok, '/') || strstr(tok, "..") || !has_txt_suffix(tok)) {
            reason = "bad name";
        } else {
            snprintf(full, sizeof(full), "%s/%s", dir, tok);
            struct stat st;
            if (!path_ok(full)) {
                reason = "bad path";
            } else if (stat(full, &st) != 0) {
                reason = "not found";
            } else if (S_ISDIR(st.st_mode)) {
                reason = "is dir";
            } else if (is_active_log_file(full)) {
                reason = "recording";       // 正在写入的日志不允许删除
            } else {
                app_fs_lock();
                int rc = remove(full);
                app_fs_unlock();
                if (rc == 0) {
                    deleted++;
                } else {
                    reason = "remove fail";
                }
            }
        }
        if (reason) {
            failed++;
        }
        if (n < cap - 160) {
            char nm[256];
            json_escape(tok, nm, sizeof(nm));
            if (reason) {
                n += snprintf(out + n, (size_t)(cap - n),
                              "%s{\"name\":\"%s\",\"ok\":false,\"err\":\"%s\"}",
                              first ? "" : ",", nm, reason);
            } else {
                n += snprintf(out + n, (size_t)(cap - n),
                              "%s{\"name\":\"%s\",\"ok\":true}",
                              first ? "" : ",", nm);
            }
            first = false;
        }
    }
    free(names);
    // 删除成功后让目录索引缓存失效，下一次列表请求立即重新扫描。
    if (deleted > 0) {
        file_index_invalidate();
    }
    n += snprintf(out + n, (size_t)(cap - n),
                  "],\"deleted\":%d,\"failed\":%d,\"ok\":%d}", deleted, failed,
                  failed == 0 ? 1 : 0);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    free(out);
    return err;
}

/* /api/port：设置某路串口参数并开关（页面「应用参数」「打开/关闭串口」）。
 * 参数：index 通道；baud 300..2000000；bits 5..8；stop 1/1.5/2；parity N/E/O；
 *       on=1 打开、0 关闭。
 * 返回 {ok:1} 或 {ok:0,err:"bad param"}。
 * 流程：校验 → 合并写入 settings 快照 → NVS 保存 → 请求 logger 重载
 *       （真正关闭/重开 UART 在 logger 上下文完成，避免跨任务操作硬件）。
 */
static esp_err_t port_post(httpd_req_t *req)
{
    char index_text[8] = {0};
    char baud_text[16] = {0};
    char bits_text[8] = {0};
    char stop_text[8] = {0};
    char parity_text[8] = {0};
    char on_text[8] = {0};
    if (!req_value(req, "index", index_text, sizeof(index_text)) ||
        !req_value(req, "baud", baud_text, sizeof(baud_text)) ||
        !req_value(req, "bits", bits_text, sizeof(bits_text)) ||
        !req_value(req, "stop", stop_text, sizeof(stop_text)) ||
        !req_value(req, "parity", parity_text, sizeof(parity_text)) ||
        !req_value(req, "on", on_text, sizeof(on_text))) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":0,\"err\":\"missing\"}");
        return ESP_OK;
    }
    int index = atoi(index_text);
    int baud = atoi(baud_text);
    uart_word_length_t bits;
    uart_stop_bits_t stop;
    uart_parity_t parity;
    if (index < 0 || index >= APP_PORT_COUNT || baud < 300 || baud > 2000000 ||
        !parse_bits(bits_text, &bits) || !parse_stop(stop_text, &stop) || !parse_parity(parity_text, &parity)) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":0,\"err\":\"bad param\"}");
        return ESP_OK;
    }
    app_settings_t cfg;
    app_settings_get(&cfg);
    cfg.port[index].baud = (uint32_t)baud;
    cfg.port[index].data_bits = bits;
    cfg.port[index].stop_bits = stop;
    cfg.port[index].parity = parity;
    cfg.port[index].enabled = strcmp(on_text, "1") == 0;
    app_settings_save(&cfg);
    app_logger_request_reload();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":1}");
}


/* /api/time：手动校时（手机/电脑端「用手机时间」也走这里）。
 * 参数：y/mo/d/h/mi/s 六项缺一不可；返回 {ok,time} 或 {ok:0,err:"bad date"}。
 * 异常处理：日期范围校验由 app_time_set 完成（非法值返回非 0，不修改 RTC）。
 */
static esp_err_t time_get(httpd_req_t *req)
{
    char y[8] = {0}, mo[8] = {0}, d[8] = {0}, h[8] = {0}, mi[8] = {0}, s[8] = {0};
    if (!req_value(req, "y", y, sizeof(y)) || !req_value(req, "mo", mo, sizeof(mo)) ||
        !req_value(req, "d", d, sizeof(d)) || !req_value(req, "h", h, sizeof(h)) ||
        !req_value(req, "mi", mi, sizeof(mi)) || !req_value(req, "s", s, sizeof(s))) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":0,\"err\":\"missing\"}");
    }
    int rc = app_time_set(atoi(y), atoi(mo), atoi(d), atoi(h), atoi(mi), atoi(s));
    char now[24] = {0};
    app_time_format(now, sizeof(now));
    char body[96];
    if (rc == 0) {
        snprintf(body, sizeof(body), "{\"ok\":1,\"time\":\"%s\"}", now);
    } else {
        snprintf(body, sizeof(body), "{\"ok\":0,\"err\":\"bad date\"}");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

/* /dl?path=：下载单个日志/配置文件。
 * 流程：校验路径 → 先 app_logger_flush() 保证最新行落盘 → 校验存在且非目录
 *       → 以 attachment 方式按 2KB 分块流式发送（内存占用与文件大小无关）。
 * 每次 fread 都短暂加 app_fs_lock，避免与日志写入/删除并发破坏 FAT。
 * 异常：路径非法 400、文件不存在/是目录 404、打开或读失败 500。
 */
static esp_err_t download_get(httpd_req_t *req)
{
    char query[160] = {0};
    char path[128] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "path", path, sizeof(path)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_FAIL;
    }
    url_decode(path);
    if (!path_ok(path)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_FAIL;
    }
    app_logger_flush();
    app_fs_lock();
    struct stat st;
    if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) {
        app_fs_unlock();
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_FAIL;
    }
    FILE *fp = fopen(path, "rb");
    app_fs_unlock();
    if (!fp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");
        return ESP_FAIL;
    }
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char disp[192];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", base);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    char *chunk = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!chunk) {
        chunk = malloc(2048);
    }
    if (!chunk) {
        fclose(fp);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
        return ESP_FAIL;
    }
    esp_err_t err = ESP_OK;
    while (1) {
        app_fs_lock();
        size_t n = fread(chunk, 1, 2048, fp);
        int ferr = ferror(fp);
        app_fs_unlock();
        if (ferr) {
            err = ESP_FAIL;
            break;
        }
        if (n == 0) {
            break;
        }
        if (httpd_resp_send_chunk(req, chunk, n) != ESP_OK) {
            err = ESP_FAIL;
            break;
        }
    }
    app_fs_lock();
    fclose(fp);
    app_fs_unlock();
    free(chunk);
    if (err == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);
    }
    return err;
}

/* 未匹配路由的兜底处理：接口/下载路径返回 404，
 * 其余请求 302 跳转到首页，配合 DNS 劫持实现 captive portal。
 */
static esp_err_t captive_404(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    if (strncmp(req->uri, "/api/", 5) == 0 || strncmp(req->uri, "/dl", 3) == 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_FAIL;
    }
    httpd_resp_set_status(req, "302 Temporary Redirect");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, "Redirect to the captive portal", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}


static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t zip_crc(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

static void join_path(char *dst, size_t n, const char *dir, const char *name)
{
    size_t i = 0;
    size_t j = 0;
    while (dir[i] && i + 1 < n) {
        dst[i] = dir[i];
        i++;
    }
    if (i + 1 < n) {
        dst[i++] = '/';
    }
    while (name[j] && i + 1 < n) {
        dst[i++] = name[j++];
    }
    dst[i] = '\0';
}

typedef struct {
    char name[96];
    uint32_t crc;
    uint32_t size;
    uint32_t offset;
} zip_ent_t;

// 校验 ZIP 内使用的相对路径（允许 "UART1/xxx.txt" 这类一层或多层子路径）：
// 禁止绝对路径、反斜杠与 ".."，每段长度与非空都要合法。
static bool rel_ok(const char *path)
{
    if (!path || !path[0] || path[0] == '/' || strchr(path, '\\') || strstr(path, "..")) {
        return false;
    }
    if (strlen(path) > 80) {
        return false;
    }
    const char *seg = path;
    for (;;) {
        const char *slash = strchr(seg, '/');
        size_t len = slash ? (size_t)(slash - seg) : strlen(seg);
        if (len == 0 || len > 60) {
            return false;
        }
        if (!slash) {
            break;
        }
        seg = slash + 1;
    }
    return true;
}

/* 收集待打包文件：从 dir 开始递归遍历全部层级，把每个文件的相对路径写入 ents。
 * 采用“目录栈”显式遍历而非函数递归：
 *   - 可支持任意多级嵌套目录，不会因为目录太深打爆 httpd 任务栈；
 *   - 栈缓冲放 PSRAM，深度上限 ZIP_DIR_STACK_MAX（远超实际日志结构需要）。
 * prefix 为相对 dir 的前缀，用于生成 zip 条目名（如 "UART1/20261027_210000.txt"）。
 * filter 为通道/日期/关键词筛选（可为 NULL），与列表页保持同一套筛选语义。
 * truncated 输出：true 表示因达到 maxn 上限而提前结束，调用方应提示用户缩小范围。
 */
#define ZIP_DIR_STACK_MAX 128

typedef struct {
    char path[SCAN_PATH_MAX];
    char prefix[FILE_NAME_MAX];
} zip_dir_stack_t;

static int collect_zip_all(const char *dir, const char *prefix,
                           const file_filter_t *filter, zip_ent_t *ents, int maxn,
                           int count, bool *truncated)
{
    zip_dir_stack_t *stack = heap_caps_malloc(ZIP_DIR_STACK_MAX * sizeof(zip_dir_stack_t),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!stack) {
        return count;
    }
    int sp = 0;
    snprintf(stack[sp].path, sizeof(stack[sp].path), "%s", dir);
    snprintf(stack[sp].prefix, sizeof(stack[sp].prefix), "%s", prefix ? prefix : "");
    sp++;

    while (sp > 0 && count < maxn) {
        sp--;
        // 拷贝到局部变量后再操作，避免 push 子目录时覆盖当前槽位。
        char cur_path[SCAN_PATH_MAX];
        char cur_prefix[FILE_NAME_MAX];
        snprintf(cur_path, sizeof(cur_path), "%s", stack[sp].path);
        snprintf(cur_prefix, sizeof(cur_prefix), "%s", stack[sp].prefix);

        char fpath[SCAN_PATH_MAX];
        if (!to_fat_path(cur_path, fpath, sizeof(fpath))) {
            continue;
        }
        FF_DIR dp;
        if (f_opendir(&dp, fpath) != FR_OK) {
            continue;
        }
        FILINFO fi;
        memset(&fi, 0, sizeof(fi));
        while (f_readdir(&dp, &fi) == FR_OK && fi.fname[0]) {
            const char *nm = fi.fname;
            if (nm[0] == '.') {
                continue;
            }
            if (count >= maxn) {
                *truncated = true;
                break;
            }
            bool is_dir = (fi.fattrib & AM_DIR) != 0;
            char full[SCAN_PATH_MAX];
            join_path(full, sizeof(full), cur_path, nm);
            char rel[FILE_NAME_MAX];
            join_rel(rel, sizeof(rel), cur_prefix, nm);
            if (is_dir) {
                if (sp < ZIP_DIR_STACK_MAX - 1) {
                    snprintf(stack[sp].path, sizeof(stack[sp].path), "%s", full);
                    snprintf(stack[sp].prefix, sizeof(stack[sp].prefix), "%s", rel);
                    sp++;
                }
                continue;
            }
            if (!rel_ok(rel)) {
                continue;
            }
            if (filter) {
                if (filter->channel >= 0) {
                    char tag[16];
                    snprintf(tag, sizeof(tag), "/UART%d/", filter->channel & 7);
                    if (!strstr(full, tag)) {
                        continue;
                    }
                }
                if (filter->date[0] && strncmp(nm, filter->date, 8) != 0) {
                    continue;
                }
                if (filter->kw[0] && !ci_contains(full, filter->kw)) {
                    continue;
                }
            }
            snprintf(ents[count].name, sizeof(ents[count].name), "%s", rel);
            count++;
        }
        f_closedir(&dp);
    }
    free(stack);
    return count;
}

/* 按当前目录/选中列表收集待打包条目（持 app_fs_lock）。
 * 参数：
 *   dir   打包根目录；names 选中文件（"a|b|c"，可含子目录前缀）；
 *   all   true=把 dir 下所有层级的文件全部打包（文件夹下载）；
 *   filter 为 all 模式下的筛选条件（可为 NULL）；
 *   maxn  输出数组容量；truncated 输出是否因容量上限被截断。
 * 返回实际收集数量。文件夹下载语义：递归收集全部文件（含子目录），
 *           不再区分“仅日志”，保证压缩包完整还原目录结构。
 */
static int collect_zip_names(const char *dir, const char *names, bool all,
                             const file_filter_t *filter,
                             zip_ent_t *ents, int maxn, bool *truncated)
{
    int count = 0;
    app_fs_lock();
    if (all) {
        // 文件夹下载：递归遍历 dir 下所有层级文件。
        count = collect_zip_all(dir, "", filter, ents, maxn, 0, truncated);
    } else if (names) {
        char *buf = strdup(names);
        char *save = NULL;
        char *tok = buf ? strtok_r(buf, "|", &save) : NULL;
        while (tok && count < maxn) {
            if (rel_ok(tok)) {
                snprintf(ents[count].name, sizeof(ents[count].name), "%s", tok);
                count++;
            }
            tok = strtok_r(NULL, "|", &save);
        }
        free(buf);
        if (tok) {
            *truncated = true;
        }
    }
    app_fs_unlock();
    return count;
}

/* /api/zip：把选中文件（names=a|b|c）或整目录（all=1）打包成 ZIP 下载。
 * 参数：dir 目录；names 以 "|" 分隔的相对文件名；all=1 表示全部；
 *       channel/date/kw 与列表页同语义，仅作用于 all=1（导出“当前筛选结果”）。
 * 返回：ZIP 流（application/zip）；超出打包上限返回 413 并提示缩小筛选范围。
 * 实现要点：
 *   1. 先 app_logger_flush() 保证活动日志最新内容进入文件；
 *   2. 采用 STORE（不压缩）方式，逐文件边读边算 CRC，避免占用大内存；
 *   3. 先发本地文件头，再发 2KB 数据块，最后统一写中央目录与 EOCD；
 *   4. 文件名用原始 UTF-8（中文目录/文件名可正常解压）。
 * 限制：单次最多 ZIP_MAX 个文件；读失败的文件会被跳过（返回其余文件）。
 */
static esp_err_t zip_get(httpd_req_t *req)
{
    // 支持一次下载整页（最多 100 条）记录：查询串与文件名列表缓冲相应放大。
    char *query = heap_caps_malloc(4600, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!query) {
        query = malloc(4600);
    }
    char dir[160] = "/sdcard";
    char all[8] = {0};
    char *names = heap_caps_malloc(3600, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!names) {
        names = malloc(3600);
    }
    zip_ent_t *ents = NULL;
    uint8_t *chunk = NULL;
    int count = 0;
    uint32_t offset = 0;
    uint32_t cd_offset = 0;
    bool truncated = false;
    esp_err_t err = ESP_OK;
    if (!query || !names) {
        free(query);
        free(names);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
        return ESP_FAIL;
    }
    names[0] = '\0';
    if (httpd_req_get_url_query_str(req, query, 4600) == ESP_OK) {
        char value[160];
        if (httpd_query_key_value(query, "dir", value, sizeof(value)) == ESP_OK) {
            url_decode(value);
            if (path_ok(value)) {
                snprintf(dir, sizeof(dir), "%s", value);
            }
        }
        httpd_query_key_value(query, "all", all, sizeof(all));
        if (httpd_query_key_value(query, "names", names, 3600) == ESP_OK) {
            url_decode(names);
        } else {
            names[0] = '\0';
        }
    }
    free(query);
    file_filter_t filter;
    parse_filter(req, &filter);
    ents = heap_caps_calloc(ZIP_MAX, sizeof(zip_ent_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    chunk = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ents || !chunk) {
        free(ents);
        free(names);
        free(chunk);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
        return ESP_FAIL;
    }
    app_logger_flush();
    count = collect_zip_names(dir, names, strcmp(all, "1") == 0, &filter, ents, ZIP_MAX, &truncated);
    free(names);
    if (truncated) {
        // 不生成残缺 ZIP：明确提示用户按通道/日期/关键词缩小范围后重试。
        free(ents);
        free(chunk);
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_send(req, "too many files, narrow filters", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (count <= 0) {
        free(ents);
        free(chunk);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no files");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/zip");
    // 下载文件名按目录名命名：下载某个文件夹时压缩包名为 <文件夹名>.zip，
    // 便于用户识别；根目录时用 sdcard.zip。
    {
        const char *base = strrchr(dir, '/');
        const char *folder = (base && base[1]) ? base + 1 : "sdcard";
        char dispname[128];
        snprintf(dispname, sizeof(dispname), "attachment; filename=\"%s.zip\"", folder);
        httpd_resp_set_hdr(req, "Content-Disposition", dispname);
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    for (int i = 0; i < count && err == ESP_OK; i++) {
        char full[FILE_PATH_MAX];   // dir(≤160) + 相对名(≤96) 必须完整拼接，不能用 256 截断
        uint8_t hdr[30];
        uint8_t desc[16];
        uint16_t namelen = (uint16_t)strlen(ents[i].name);
        FILE *fp;
        uint32_t crc = 0;
        uint32_t size = 0;
        join_path(full, sizeof(full), dir, ents[i].name);
        ents[i].offset = offset;
        put_u32(hdr, 0x04034b50);
        put_u16(hdr + 4, 20);
        put_u16(hdr + 6, 0x0008);
        put_u16(hdr + 8, 0);
        put_u16(hdr + 10, 0);
        put_u16(hdr + 12, 0);
        put_u32(hdr + 14, 0);
        put_u32(hdr + 18, 0);
        put_u32(hdr + 22, 0);
        put_u16(hdr + 26, namelen);
        put_u16(hdr + 28, 0);
        if (httpd_resp_send_chunk(req, (const char *)hdr, sizeof(hdr)) != ESP_OK ||
            httpd_resp_send_chunk(req, ents[i].name, namelen) != ESP_OK) {
            err = ESP_FAIL;
            break;
        }
        offset += sizeof(hdr) + namelen;
        app_fs_lock();
        fp = fopen(full, "rb");
        app_fs_unlock();
        if (fp) {
            while (err == ESP_OK) {
                size_t n;
                app_fs_lock();
                n = fread(chunk, 1, 2048, fp);
                app_fs_unlock();
                if (n == 0) {
                    break;
                }
                crc = zip_crc(crc, chunk, n);
                size += (uint32_t)n;
                if (httpd_resp_send_chunk(req, (const char *)chunk, n) != ESP_OK) {
                    err = ESP_FAIL;
                    break;
                }
                offset += (uint32_t)n;
            }
            app_fs_lock();
            fclose(fp);
            app_fs_unlock();
        }
        ents[i].crc = crc;
        ents[i].size = size;
        put_u32(desc, 0x08074b50);
        put_u32(desc + 4, crc);
        put_u32(desc + 8, size);
        put_u32(desc + 12, size);
        if (httpd_resp_send_chunk(req, (const char *)desc, sizeof(desc)) != ESP_OK) {
            err = ESP_FAIL;
            break;
        }
        offset += sizeof(desc);
    }
    cd_offset = offset;
    for (int i = 0; i < count && err == ESP_OK; i++) {
        uint8_t cd[46];
        uint16_t namelen = (uint16_t)strlen(ents[i].name);
        put_u32(cd, 0x02014b50);
        put_u16(cd + 4, 20);
        put_u16(cd + 6, 20);
        put_u16(cd + 8, 0x0008);
        put_u16(cd + 10, 0);
        put_u16(cd + 12, 0);
        put_u16(cd + 14, 0);
        put_u32(cd + 16, ents[i].crc);
        put_u32(cd + 20, ents[i].size);
        put_u32(cd + 24, ents[i].size);
        put_u16(cd + 28, namelen);
        put_u16(cd + 30, 0);
        put_u16(cd + 32, 0);
        put_u16(cd + 34, 0);
        put_u16(cd + 36, 0);
        put_u32(cd + 38, 0);
        put_u32(cd + 42, ents[i].offset);
        if (httpd_resp_send_chunk(req, (const char *)cd, sizeof(cd)) != ESP_OK ||
            httpd_resp_send_chunk(req, ents[i].name, namelen) != ESP_OK) {
            err = ESP_FAIL;
            break;
        }
        offset += sizeof(cd) + namelen;
    }
    if (err == ESP_OK) {
        uint8_t end[22];
        put_u32(end, 0x06054b50);
        put_u16(end + 4, 0);
        put_u16(end + 6, 0);
        put_u16(end + 8, (uint16_t)count);
        put_u16(end + 10, (uint16_t)count);
        put_u32(end + 12, offset - cd_offset);
        put_u32(end + 16, cd_offset);
        put_u16(end + 20, 0);
        if (httpd_resp_send_chunk(req, (const char *)end, sizeof(end)) != ESP_OK) {
            err = ESP_FAIL;
        }
    }
    if (err == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);
    }
    free(ents);
    free(chunk);
    return err;
}


/* 实时文本的 JSON 转义（在 json_escape 基础上把换行转成 \n，
 * 保证多行日志在 JSON 字符串中合法；\r 与其它控制字符直接丢弃）。 */
static void json_escape_text(const char *src, char *dst, size_t dst_len)
{
    size_t n = 0;
    for (size_t i = 0; src && src[i] && n + 3 < dst_len; i++) {
        unsigned char ch = (unsigned char)src[i];
        if (ch == '"' || ch == '\\') {
            dst[n++] = '\\';
            dst[n++] = (char)ch;
        } else if (ch == '\n') {
            dst[n++] = '\\';
            dst[n++] = 'n';
        } else if (ch == '\r') {
            continue;
        } else if (ch < 0x20) {
            continue;
        } else {
            dst[n++] = (char)ch;
        }
    }
    dst[n] = '\0';
}

/* /api/live?0=<cur>&1=<cur>&2=<cur>：按通道返回自游标以来的增量文本。
 * 参数：键为通道号，值为上次返回的 next 游标（缺省表示该路不订阅）。
 * 返回：{"ports":[{"i","next","text"}...]}；每路文本上限 700 字节，
 *       超出部分被截断但游标仍然推进，避免前端反复拉同一段。
 * 说明：网页每 500ms 轮询一次，未请求的通道不产生任何数据。
 */
static esp_err_t live_get(httpd_req_t *req)
{
    char *body = malloc(4600);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
        return ESP_FAIL;
    }
    int n = snprintf(body, 4600, "{\"ports\":[");
    bool first = true;
    for (int i = 0; i < APP_PORT_COUNT; i++) {
        char key[4];
        char since_text[16] = {0};
        char text[700];
        char escaped[1100];
        uint32_t next = 0;
        snprintf(key, sizeof(key), "%d", i);
        if (!req_value(req, key, since_text, sizeof(since_text))) {
            continue;
        }
        app_logger_live_get(i, (uint32_t)strtoul(since_text, NULL, 10), text, sizeof(text), &next);
        json_escape_text(text, escaped, sizeof(escaped));
        if (n < 4300) {
            n += snprintf(body + n, 4600 - (size_t)n,
                          "%s{\"i\":%d,\"next\":%lu,\"text\":\"%s\"}",
                          first ? "" : ",", i, (unsigned long)next, escaped);
            first = false;
        }
    }
    snprintf(body + n, 4600 - (size_t)n, "]}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    free(body);
    return err;
}

/* /api/config：配置管理入口。
 * 无 op 参数时返回状态（是否存在 config.json/.bak、上次操作结果与提示）；
 * op=export 立即导出；op=reload 从卡重载并触发串口重开；
 * op=restore 恢复 .bak 并触发串口重开。返回 {ok,msg|err}。
 */
static esp_err_t config_get(httpd_req_t *req)
{
    char op[12] = {0};
    char body[320];
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    if (!req_value(req, "op", op, sizeof(op))) {
        app_config_status_t st;
        app_config_get_status(&st);
        snprintf(body, sizeof(body),
                 "{\"ok\":true,\"present\":%s,\"backup\":%s,\"last_ok\":%s,\"msg\":\"%s\"}",
                 st.present ? "true" : "false", st.backup_present ? "true" : "false",
                 st.last_ok ? "true" : "false", st.msg);
        return httpd_resp_sendstr(req, body);
    }

    bool ok = false;
    const char *msg = "失败";
    if (!strcmp(op, "export")) {
        ok = app_config_export() == 0;
        msg = ok ? "已导出" : "导出失败";
    } else if (!strcmp(op, "reload")) {
        int rc = app_config_load();
        ok = rc == 0;
        msg = ok ? "已重载" : (rc == 1 ? "config.json不存在" : "配置无效");
        if (ok) {
            app_logger_request_reload();
        }
    } else if (!strcmp(op, "restore")) {
        ok = app_config_restore_backup() == 0;
        msg = ok ? "备份已恢复" : "恢复失败";
        if (ok) {
            app_logger_request_reload();
        }
    } else {
        msg = "未知操作";
    }
    snprintf(body, sizeof(body), "{\"ok\":%s,\"%s\":\"%s\"}",
             ok ? "true" : "false", ok ? "msg" : "err", msg);
    return httpd_resp_sendstr(req, body);
}

/* /api/config_upload（POST）：把浏览器上传的 config.json 导入配置。
 * 参数：请求体为 JSON 文本（上限 APP_CONFIG_FILE_BYTES，超出即拒绝）。
 * 返回 {ok:1} 或 {ok:0,err}；成功后触发串口参数重载。
 * 异常处理：接收超时按 HTTPD_SOCK_ERR_TIMEOUT 继续等；其它错误终止读取；
 *           恰好读满上限时再多读 1 字节判断是否真的超长，避免误判。
 */
static esp_err_t config_upload_post(httpd_req_t *req)
{
    char *buf = heap_caps_malloc(APP_CONFIG_FILE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
        return ESP_FAIL;
    }
    int total = 0;
    bool too_big = false;

    while (total < APP_CONFIG_FILE_BYTES) {
        int r = httpd_req_recv(req, buf + total, APP_CONFIG_FILE_BYTES - total);
        if (r < 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            break;
        }
        if (r == 0) {
            break;
        }
        total += r;
    }
    if (total == APP_CONFIG_FILE_BYTES) {
        char one;
        too_big = httpd_req_recv(req, &one, 1) > 0;
    }

    int rc = too_big ? -1 : app_config_import_text(buf, (size_t)total);
    free(buf);
    if (rc == 0) {
        app_logger_request_reload();
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, rc == 0 ? "{\"ok\":1}" : "{\"ok\":0,\"err\":\"配置无效或过大\"}");
}

/* 启动 Web 服务器并注册全部路由（幂等：已启动则直接返回）。
 * 配置要点：
 *   max_open_sockets=4 + lru_purge_enable：手机浏览器最多开 6 连接，
 *     超出时回收最久未用的 socket，避免“请求一直转圈”；
 *   stack_size=12288：/api/files 需要“递归扫描目录 + 解析参数 + 组装 JSON”，
 *     8KB 会栈溢出（曾导致点刷新即重启），这里留足余量；
 *   max_uri_handlers=16：当前注册 14 个 URI，新增接口必须同步调大；
 *   recv/send 超时放宽，适配手机端弱网。
 * 失败处理：内部 RAM 不足会导致 httpd_start 失败，此处只记录错误并保留
 *           s_server=NULL；应检查 SPIRAM_TRY_ALLOCATE_WIFI_LWIP 与 LCD
 *           DMA 预留是否过大。
 */
void app_httpd_start(void)
{
    if (s_server) {
        return;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.stack_size = 12288;
    config.max_uri_handlers = 16;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 20;
    esp_err_t rc = httpd_start(&s_server, &config);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed: %s", esp_err_to_name(rc));
        s_server = NULL;
        return;
    }
    const httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get};
    const httpd_uri_t status = {.uri = "/api/status", .method = HTTP_GET, .handler = status_get};
    const httpd_uri_t files = {.uri = "/api/files", .method = HTTP_GET, .handler = files_get};
    const httpd_uri_t del = {.uri = "/api/delete", .method = HTTP_GET, .handler = delete_get};
    const httpd_uri_t port = {.uri = "/api/port", .method = HTTP_GET, .handler = port_post};
    const httpd_uri_t clock = {.uri = "/api/time", .method = HTTP_GET, .handler = time_get};
    const httpd_uri_t dl = {.uri = "/dl", .method = HTTP_GET, .handler = download_get};
    const httpd_uri_t zip = {.uri = "/api/zip", .method = HTTP_GET, .handler = zip_get};
    const httpd_uri_t live = {.uri = "/api/live", .method = HTTP_GET, .handler = live_get};
    const httpd_uri_t bridge_st = {.uri = "/api/bridge", .method = HTTP_GET, .handler = bridge_status_get};
    const httpd_uri_t bridge_tcp = {.uri = "/api/bridge_tcp", .method = HTTP_GET, .handler = bridge_tcp_post};
    const httpd_uri_t bridge_ble = {.uri = "/api/bridge_ble", .method = HTTP_GET, .handler = bridge_ble_post};
    const httpd_uri_t cfg = {.uri = "/api/config", .method = HTTP_GET, .handler = config_get};
    const httpd_uri_t cfg_up = {.uri = "/api/config_upload", .method = HTTP_POST, .handler = config_upload_post};
    httpd_register_uri_handler(s_server, &root);
    httpd_register_uri_handler(s_server, &status);
    httpd_register_uri_handler(s_server, &files);
    httpd_register_uri_handler(s_server, &del);
    httpd_register_uri_handler(s_server, &port);
    httpd_register_uri_handler(s_server, &clock);
    httpd_register_uri_handler(s_server, &dl);
    httpd_register_uri_handler(s_server, &zip);
    httpd_register_uri_handler(s_server, &live);
    httpd_register_uri_handler(s_server, &bridge_st);
    httpd_register_uri_handler(s_server, &bridge_tcp);
    httpd_register_uri_handler(s_server, &bridge_ble);
    httpd_register_uri_handler(s_server, &cfg);
    httpd_register_uri_handler(s_server, &cfg_up);
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, captive_404);
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);

    dns_server_config_t dns = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    if (!start_dns_server(&dns)) {
        ESP_LOGW(TAG, "dns server failed, open http://192.168.4.1");
    }
    ESP_LOGI(TAG, "web ready http://192.168.4.1");
}
