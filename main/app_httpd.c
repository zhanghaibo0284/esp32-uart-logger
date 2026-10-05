#include "app_httpd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#ifndef DT_DIR
#define DT_DIR 4
#endif
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "dns_server.h"
#include "app_logger.h"
#include "app_settings.h"
#include "app_sd.h"
#include "app_time.h"
#include "app_wifi.h"
#include "app_bridge.h"

static const char *TAG = "httpd";
static httpd_handle_t s_server;
static const char WEB_PAGE[] = "<!DOCTYPE html>\n<html lang=\"zh\">\n<head>\n<meta charset=\"utf-8\">\n<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n<title>串口记录仪</title>\n<style>\nbody{font:16px/1.45 -apple-system,BlinkMacSystemFont,\"Segoe UI\",sans-serif;margin:0;background:#0f1419;color:#e8eef5}\nheader,main{padding:12px}\nh1{font-size:20px;margin:0 0 6px}\nh2{font-size:16px;margin:0 0 8px}\n.card{background:#1b2430;border-radius:10px;padding:10px;margin:10px 0}\nlabel{display:block;margin:6px 0;color:#b7c3d1}\nselect,input,button{font-size:16px}\ninput{width:64px;padding:6px;border-radius:6px;border:0}\nbutton{background:#2f6fed;color:#fff;border:0;border-radius:8px;padding:12px 14px;margin:6px 6px 0 0}\n.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}\n.muted{color:#9aa8b8}\n.line{display:flex;align-items:center;gap:8px;margin:8px 0}\n.item{flex:1;display:block;color:#d7e6ff;text-decoration:none;background:#243044;border-radius:8px;padding:12px}\n.pick{width:22px;height:22px}\n#crumb a{color:#8ec5ff;margin-right:4px}\n.msg{min-height:1.2em;color:#ffd27a}\n#live{height:240px;overflow:auto;background:#0b0f14;padding:8px;white-space:pre-wrap;font:13px/1.35 ui-monospace,monospace}\n</style>\n</head>\n<body>\n<header>\n<h1>串口记录仪</h1>\n<div id=\"info\" class=\"muted\">正在读取...</div>\n</header>\n<main>\n<div id=\"ports\"></div>\n<div class=\"card\">\n<h2>串口透传</h2>\n<div class=\"muted\">WiFi TCP：用电脑 TCP 工具连接 192.168.4.1 对应端口即可双向透传。蓝牙为 BLE UART(NUS)，手机需用 BLE 调试工具(如 nRF Connect) 连接。</div>\n<div id=\"bridge\"></div>\n<div id=\"bridge-msg\" class=\"msg\"></div>\n</div>\n<div class=\"card\">\n<h2>实时数据</h2>\n<div class=\"row\">\n<label><input id=\"lv0\" class=\"pick\" type=\"checkbox\"> U0</label>\n<label><input id=\"lv1\" class=\"pick\" type=\"checkbox\" checked> J2</label>\n<label><input id=\"lv2\" class=\"pick\" type=\"checkbox\" checked> J3</label>\n<button type=\"button\" id=\"lv-clear\">清空</button>\n</div>\n<pre id=\"live\"></pre>\n</div>\n<div class=\"card\">\n<h2>校时</h2>\n<div class=\"row\">\n<label>年 <input id=\"yy\" inputmode=\"numeric\"></label>\n<label>月 <input id=\"mo\" inputmode=\"numeric\"></label>\n<label>日 <input id=\"dd\" inputmode=\"numeric\"></label>\n</div>\n<div class=\"row\">\n<label>时 <input id=\"hh\" inputmode=\"numeric\"></label>\n<label>分 <input id=\"mm\" inputmode=\"numeric\"></label>\n<label>秒 <input id=\"ss\" inputmode=\"numeric\"></label>\n</div>\n<button type=\"button\" id=\"btn-time\">校时</button>\n<button type=\"button\" id=\"btn-phone\">用手机时间</button>\n<div id=\"time-msg\" class=\"msg\"></div>\n</div>\n<div class=\"card\">\n<h2>日志文件</h2>\n<div id=\"crumb\" class=\"muted\"></div>\n<button type=\"button\" id=\"btn-all\">全选</button>\n<button type=\"button\" id=\"btn-zip\">下载选中</button>\n<button type=\"button\" id=\"btn-zipall\">下载全部</button>\n<div id=\"files\"></div>\n<div id=\"file-msg\" class=\"msg\"></div>\n</div>\n</main>\n<script>\nconst names=[\"U0 板载\",\"J2\",\"J3\"];\nconst bauds=[115200,9600,19200,38400,57600,230400,460800,921600,4800,2400,1200];\nlet cur=\"/sdcard\";\nlet ports=[];\nfunction opts(list,val){\n  return list.map(function(v){\n    return '<option value=\"'+v+'\"'+(String(v)===String(val)?' selected':'')+'>'+v+'</option>';\n  }).join('');\n}\nfunction show(id,text){var el=document.getElementById(id); if(el) el.textContent=text||'';}\nfunction fillTime(text){\n  var m=/(\\d{4})-(\\d{2})-(\\d{2}) (\\d{2}):(\\d{2}):(\\d{2})/.exec(text||'');\n  if(!m) return;\n  document.getElementById('yy').value=m[1];\n  document.getElementById('mo').value=String(Number(m[2]));\n  document.getElementById('dd').value=String(Number(m[3]));\n  document.getElementById('hh').value=String(Number(m[4]));\n  document.getElementById('mm').value=String(Number(m[5]));\n  document.getElementById('ss').value=String(Number(m[6]));\n}\nfunction renderPorts(){\n  document.getElementById('ports').innerHTML=ports.map(function(p,i){\n    var st=p.open?('已打开 TX '+p.tx+' RX '+p.rx):(p.err?('失败 '+p.err):'已关闭');\n    return '<div class=\"card\" id=\"p'+i+'\"><b>'+names[i]+'</b> <span class=\"st\">'+st+'</span>'\n      +'<label>波特率 <select class=\"baud\">'+opts(bauds,p.baud)+'</select></label>'\n      +'<div class=\"row\"><label>数据位 <select class=\"bits\">'+opts([8,7,6,5],p.bits)+'</select></label>'\n      +'<label>停止位 <select class=\"stop\">'+opts(['1','1.5','2'],p.stop)+'</select></label>'\n      +'<label>校验 <select class=\"par\">'+opts(['N','E','O'],p.parity)+'</select></label></div>'\n      +'<button type=\"button\" data-act=\"toggle\" data-i=\"'+i+'\">'+(p.on?'关闭串口':'打开串口')+'</button>'\n      +'<button type=\"button\" data-act=\"apply\" data-i=\"'+i+'\">应用参数</button>'\n      +'<div class=\"msg\" id=\"m'+i+'\"></div></div>';\n  }).join('');\n}\nasync function loadStatus(fill){\n  try{\n    const s=await (await fetch('/api/status')).json();\n    show('info', s.time+'  '+(s.sd?('SD '+s.free_mb+' MB'):'无卡')+'  '+s.ssid);\n    if(fill){\n      ports=s.ports;\n      renderPorts();\n      fillTime(s.time);\n    }else{\n      s.ports.forEach(function(p,i){\n        ports[i].open=p.open; ports[i].tx=p.tx; ports[i].rx=p.rx; ports[i].err=p.err; ports[i].on=p.on;\n        var el=document.querySelector('#p'+i+' .st');\n        if(el) el.textContent=p.open?('已打开 TX '+p.tx+' RX '+p.rx):(p.err?('失败 '+p.err):'已关闭');\n        var btn=document.querySelector('#p'+i+' button[data-act=\"toggle\"]');\n        if(btn) btn.textContent=p.on?'关闭串口':'打开串口';\n      });\n    }\n  }catch(e){\n    show('info','读取失败');\n  }\n}\nfunction cardParams(i, on){\n  var card=document.getElementById('p'+i);\n  return new URLSearchParams({\n    index:String(i),\n    baud:card.querySelector('.baud').value,\n    bits:card.querySelector('.bits').value,\n    stop:card.querySelector('.stop').value,\n    parity:card.querySelector('.par').value,\n    on:on\n  });\n}\nasync function savePort(i, on){\n  show('m'+i,'请稍候');\n  try{\n    const r=await fetch('/api/port?'+cardParams(i,on).toString());\n    const j=await r.json();\n    show('m'+i, j.ok?(on==='1'?'已打开':'已关闭'):(j.err||'失败'));\n    if(j.ok){ ports[i].on=(on==='1'); }\n    await loadStatus(false);\n  }catch(e){\n    show('m'+i,'无响应');\n  }\n}\ndocument.getElementById('ports').addEventListener('click', function(ev){\n  var node=ev.target;\n  while(node && node.tagName!=='BUTTON') node=node.parentNode;\n  if(!node || !node.dataset) return;\n  var i=Number(node.dataset.i);\n  if(node.dataset.act==='toggle'){\n    savePort(i, ports[i] && ports[i].on ? '0' : '1');\n  }else if(node.dataset.act==='apply'){\n    savePort(i, (ports[i] && ports[i].on) ? '1' : '0');\n  }\n});\ndocument.getElementById('btn-time').addEventListener('click', async function(){\n  show('time-msg','请稍候');\n  var body=new URLSearchParams({\n    y:document.getElementById('yy').value,\n    mo:document.getElementById('mo').value,\n    d:document.getElementById('dd').value,\n    h:document.getElementById('hh').value,\n    mi:document.getElementById('mm').value,\n    s:document.getElementById('ss').value\n  });\n  try{\n    const r=await fetch('/api/time?'+body.toString());\n    const j=await r.json();\n    show('time-msg', j.ok?('校时成功 '+j.time):(j.err||'日期无效'));\n    if(j.ok) show('info', j.time);\n  }catch(e){\n    show('time-msg','无响应');\n  }\n});\nfunction safeDir(path){\n  if(!path || path.indexOf('/sdcard')!==0 || path.indexOf('..')>=0) return '/sdcard';\n  return path;\n}\nfunction upPath(path){\n  path=safeDir(path);\n  if(path==='/sdcard') return '/sdcard';\n  var i=path.lastIndexOf('/');\n  return i<=7?'/sdcard':path.slice(0,i);\n}\nfunction renderCrumb(path){\n  var box=document.getElementById('crumb');\n  box.innerHTML='';\n  var acc='';\n  path.split('/').forEach(function(part){\n    if(!part) return;\n    acc+='/'+part;\n    if(box.childNodes.length){\n      var sep=document.createElement('span');\n      sep.textContent=' / ';\n      box.appendChild(sep);\n    }\n    var a=document.createElement('a');\n    a.textContent=part;\n    a.href='/?dir='+encodeURIComponent(acc);\n    a.setAttribute('data-dir','1');\n    a.setAttribute('data-path',acc);\n    box.appendChild(a);\n  });\n}\nasync function loadFiles(path){\n  cur=safeDir(path);\n  show('file-msg','');\n  try{\n    const r=await fetch('/api/files?path='+encodeURIComponent(cur));\n    const s=await r.json();\n    if(s.err){ show('file-msg', s.err); }\n    var shown=safeDir(s.path||cur);\n    cur=shown;\n    renderCrumb(shown);\n    var box=document.getElementById('files');\n    box.innerHTML='';\n    if(shown!=='/sdcard'){\n      var back=document.createElement('a');\n      back.className='item';\n      back.textContent='.. 返回';\n      back.href='/?dir='+encodeURIComponent(upPath(shown));\n      back.setAttribute('data-dir','1');\n      back.setAttribute('data-path', upPath(shown));\n      var line0=document.createElement('div');\n      line0.className='line';\n      line0.appendChild(back);\n      box.appendChild(line0);\n    }\n    (s.items||[]).forEach(function(it){\n      var full=shown+'/'+it.name;\n      var line=document.createElement('div');\n      line.className='line';\n      if(!it.dir){\n        var ck=document.createElement('input');\n        ck.type='checkbox';\n        ck.className='pick';\n        ck.setAttribute('data-name', it.name);\n        line.appendChild(ck);\n      }\n      var a=document.createElement('a');\n      a.className='item';\n      if(it.dir){\n        a.textContent=it.name+'/';\n        a.href='/?dir='+encodeURIComponent(full);\n        a.setAttribute('data-dir','1');\n        a.setAttribute('data-path', full);\n      }else{\n        a.textContent=it.name+'  '+it.size;\n        a.href='/dl?path='+encodeURIComponent(full);\n      }\n      line.appendChild(a);\n      box.appendChild(line);\n    });\n    if(!(s.items||[]).length){\n      var empty=document.createElement('div');\n      empty.className='muted';\n      empty.textContent='空';\n      box.appendChild(empty);\n    }\n  }catch(e){\n    show('file-msg','目录读取失败');\n  }\n}\nfunction onDirClick(ev){\n  var node=ev.target;\n  while(node && node.tagName!=='A') node=node.parentNode;\n  if(!node || node.getAttribute('data-dir')!=='1') return;\n  ev.preventDefault();\n  loadFiles(node.getAttribute('data-path'));\n}\ndocument.getElementById('crumb').addEventListener('click', onDirClick);\ndocument.getElementById('files').addEventListener('click', onDirClick);\ndocument.getElementById('btn-all').addEventListener('click', function(){\n  var boxes=document.querySelectorAll('#files .pick');\n  var mark=false;\n  for(var i=0;i<boxes.length;i++){ if(!boxes[i].checked) mark=true; }\n  for(var j=0;j<boxes.length;j++) boxes[j].checked=mark;\n});\nfunction selectedNames(){\n  var names=[];\n  var boxes=document.querySelectorAll('#files .pick');\n  for(var i=0;i<boxes.length;i++){\n    if(boxes[i].checked) names.push(boxes[i].getAttribute('data-name'));\n  }\n  return {names:names, total:boxes.length};\n}\ndocument.getElementById('btn-zip').addEventListener('click', function(){\n  var sel=selectedNames();\n  if(!sel.names.length){ show('file-msg','请选择文件'); return; }\n  if(sel.names.length===sel.total){\n    window.location.href='/api/zip?dir='+encodeURIComponent(cur)+'&all=1';\n    return;\n  }\n  var q='/api/zip?dir='+encodeURIComponent(cur)+'&names='+encodeURIComponent(sel.names.join('|'));\n  if(q.length>1500){ show('file-msg','选中太多,请用下载全部'); return; }\n  window.location.href=q;\n});\ndocument.getElementById('btn-zipall').addEventListener('click', function(){\n  window.location.href='/api/zip?dir='+encodeURIComponent(cur)+'&all=1';\n});\ndocument.getElementById('btn-phone').addEventListener('click', async function(){\n  var d=new Date();\n  document.getElementById('yy').value=d.getFullYear();\n  document.getElementById('mo').value=d.getMonth()+1;\n  document.getElementById('dd').value=d.getDate();\n  document.getElementById('hh').value=d.getHours();\n  document.getElementById('mm').value=d.getMinutes();\n  document.getElementById('ss').value=d.getSeconds();\n  show('time-msg','请稍候');\n  var body=new URLSearchParams({\n    y:d.getFullYear(), mo:d.getMonth()+1, d:d.getDate(),\n    h:d.getHours(), mi:d.getMinutes(), s:d.getSeconds()\n  });\n  try{\n    const r=await fetch('/api/time?'+body.toString());\n    const j=await r.json();\n    show('time-msg', j.ok?('校时成功 '+j.time):(j.err||'日期无效'));\n    if(j.ok) show('info', j.time);\n  }catch(e){\n    show('time-msg','无响应');\n  }\n});\n\n\nlet liveCursor=[0,0,0];\ndocument.getElementById('lv-clear').addEventListener('click', function(){\n  document.getElementById('live').textContent='';\n  liveCursor=[0,0,0];\n});\nasync function pollLive(){\n  var q=[];\n  for(var i=0;i<3;i++){\n    var box=document.getElementById('lv'+i);\n    if(box && box.checked) q.push(i+'='+liveCursor[i]);\n  }\n  if(!q.length) return;\n  try{\n    const s=await (await fetch('/api/live?'+q.join('&'))).json();\n    var view=document.getElementById('live');\n    var stick=view.scrollTop+view.clientHeight>=view.scrollHeight-24;\n    (s.ports||[]).forEach(function(p){\n      liveCursor[p.i]=p.next;\n      if(p.text) view.textContent+=p.text;\n    });\n    if(view.textContent.length>24000) view.textContent=view.textContent.slice(-14000);\n    if(stick) view.scrollTop=view.scrollHeight;\n  }catch(e){}\n}\n\nloadStatus(true);\nvar startDir='/sdcard';\ntry{ var qs=new URLSearchParams(location.search); startDir=safeDir(qs.get('dir')||'/sdcard'); }catch(e){}\nloadFiles(startDir);\nsetInterval(function(){loadStatus(false);},2000);\nsetInterval(pollLive,500);\n\nfunction apiGet(url){ return fetch(url).then(function(r){return r.json();}); }\nfunction renderBridge(){\n  apiGet('/api/bridge').then(function(b){\n    var labels=['U0','J2','J3'];\n    var rows=b.tcp.map(function(t,i){\n      return '<div class=\"line\"><b style=\"width:34px\">'+labels[i]+'</b>'\n        +'<label>TCP端口 <input class=\"bport\" style=\"width:90px\" value=\"'+t.port+'\"></label>'\n        +'<button type=\"button\" class=\"btcp\" data-i=\"'+i+'\">'+(t.on?('停止(客户端'+t.clients+')'):'开启TCP')+'</button>'\n        +'<span class=\"muted\">收'+t.rx+' 发'+t.tx+'</span></div>';\n    }).join('');\n    var ble=b.ble;\n    rows+='<div class=\"line\"><b style=\"width:34px\">BLE</b>'\n      +'<label>绑定 <select id=\"ble-port\"><option value=\"0\">U0</option><option value=\"1\"'+(ble.port===1?' selected':'')+'>J2</option><option value=\"2\"'+(ble.port===2?' selected':'')+'>J3</option></select></label>'\n      +'<label>名称 <input id=\"ble-name\" style=\"width:120px\" value=\"'+ble.name+'\"></label>'\n      +'<button type=\"button\" id=\"b-ble\">'+(ble.on?(ble.connected?'已连接,停止':'广播中,停止'):'开启BLE')+'</button>'\n      +'<span class=\"muted\">收'+ble.rx+' 发'+ble.tx+'</span></div>';\n    document.getElementById('bridge').innerHTML=rows;\n    Array.prototype.forEach.call(document.querySelectorAll('.btcp'),function(btn){\n      btn.onclick=function(){\n        var i=btn.getAttribute('data-i');\n        var on=btn.textContent.indexOf('开启')===0;\n        var port=btn.parentElement.querySelector('.bport').value;\n        apiGet('/api/bridge_tcp?index='+i+'&on='+(on?1:0)+'&port='+port).then(function(r){\n          show('bridge-msg',r.ok?'已更新':('失败 '+(r.err||''))); renderBridge();\n        });\n      };\n    });\n    document.getElementById('b-ble').onclick=function(){\n      var on=document.getElementById('b-ble').textContent.indexOf('开启')===0;\n      var p=document.getElementById('ble-port').value;\n      var nm=encodeURIComponent(document.getElementById('ble-name').value);\n      apiGet('/api/bridge_ble?index='+p+'&on='+(on?1:0)+'&name='+nm).then(function(r){\n        show('bridge-msg',r.ok?'已更新':('失败 '+(r.err||''))); renderBridge();\n      });\n    };\n  });\n}\n\nsetInterval(renderBridge, 3000);\n</script>\n</body>\n</html>";

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

static esp_err_t bridge_status_get(httpd_req_t *req)
{
    bridge_info_t info;
    app_bridge_get(&info);
    char body[1200];
    int n = snprintf(body, sizeof(body),
                     "{\"tcp\":[");
    for (int i = 0; i < APP_PORT_COUNT && n > 0; i++) {
        n += snprintf(body + n, sizeof(body) - (size_t)n,
                      "%s{\"on\":%s,\"port\":%u,\"clients\":%d,\"rx\":%lu,\"tx\":%lu}",
                      i ? "," : "",
                      info.tcp_on[i] ? "true" : "false",
                      info.tcp_port[i], info.tcp_clients[i],
                      (unsigned long)info.tcp_rx[i],
                      (unsigned long)info.tcp_tx[i]);
    }
    n += snprintf(body + n, sizeof(body) - (size_t)n,
                  "],\"ble\":{\"on\":%s,\"connected\":%s,\"port\":%d,\"rx\":%lu,\"tx\":%lu,\"name\":\"%s\"}}",
                  info.ble_on ? "true" : "false",
                  info.ble_connected ? "true" : "false",
                  info.ble_port,
                  (unsigned long)info.ble_rx,
                  (unsigned long)info.ble_tx,
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
                      "%s{\"open\":%s,\"on\":%s,\"tx\":%lu,\"rx\":%lu,\"err\":%d,\"baud\":%lu,\"bits\":%d,\"stop\":\"%s\",\"parity\":\"%s\"}",
                      i ? "," : "",
                      view.port[i].open ? "true" : "false",
                      cfg.port[i].enabled ? "true" : "false",
                      (unsigned long)view.port[i].tx_bytes,
                      (unsigned long)view.port[i].rx_bytes,
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

static int hex_nibble(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

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

static bool req_value(httpd_req_t *req, const char *key, char *out, size_t out_len)
{
    char query[320] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    if (httpd_query_key_value(query, key, out, out_len) != ESP_OK) {
        return false;
    }
    url_decode(out);
    return true;
}

static esp_err_t files_get(httpd_req_t *req)
{
    char path[160] = "/sdcard";
    char value[160] = {0};
    if (req_value(req, "path", value, sizeof(value)) && path_ok(value)) {
        snprintf(path, sizeof(path), "%s", value);
    }
    char escaped[160];
    json_escape(path, escaped, sizeof(escaped));
    char *json = heap_caps_malloc(16384, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!json) {
        json = malloc(8192);
    }
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
        return ESP_FAIL;
    }
    int cap = 12000;
    snprintf(json, cap, "{\"path\":\"%s\",\"items\":[", escaped);
    bool first = true;
    int count = 0;
    app_fs_lock();
    DIR *dir = opendir(path);
    if (!dir) {
        snprintf(json, cap, "{\"path\":\"%s\",\"items\":[],\"err\":\"open fail\"}", escaped);
    } else {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL && count < 100) {
            if (ent->d_name[0] == '.') {
                continue;
            }
            char full[512];
            snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
            struct stat st;
            bool is_dir = (ent->d_type == DT_DIR);
            long size = 0;
            if (stat(full, &st) == 0) {
                if (S_ISDIR(st.st_mode)) {
                    is_dir = true;
                }
                size = (long)st.st_size;
            }
            if (!is_dir && strchr(ent->d_name, '.') == NULL) {
                is_dir = true;
            }
            char name[96];
            json_escape(ent->d_name, name, sizeof(name));
            char item[220];
            snprintf(item, sizeof(item), "%s{\"name\":\"%s\",\"dir\":%s,\"size\":%ld}",
                     first ? "" : ",", name, is_dir ? "true" : "false", size);
            if ((int)strlen(json) + (int)strlen(item) + 4 >= cap) {
                break;
            }
            strcat(json, item);
            first = false;
            count++;
        }
        closedir(dir);
        strcat(json, "]}");
    }
    app_fs_unlock();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return err;
}

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

static bool name_ok(const char *name)
{
    size_t n;
    if (!name || !name[0]) {
        return false;
    }
    n = strlen(name);
    if (n > 60 || strstr(name, "..") || strchr(name, '/') || strchr(name, '\\')) {
        return false;
    }
    return true;
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
    char name[64];
    uint32_t crc;
    uint32_t size;
    uint32_t offset;
} zip_ent_t;

static int collect_zip_names(const char *dir, const char *names, bool all, zip_ent_t *ents, int maxn)
{
    int count = 0;
    app_fs_lock();
    if (all) {
        DIR *dp = opendir(dir);
        if (dp) {
            struct dirent *ent;
            while ((ent = readdir(dp)) != NULL && count < maxn) {
                char full[256];
                struct stat st;
                if (ent->d_name[0] == '.' || !name_ok(ent->d_name)) {
                    continue;
                }
                join_path(full, sizeof(full), dir, ent->d_name);
                if (stat(full, &st) != 0 || S_ISDIR(st.st_mode) || ent->d_type == DT_DIR) {
                    continue;
                }
                size_t k = 0;
                while (ent->d_name[k] && k + 1 < sizeof(ents[count].name)) {
                    ents[count].name[k] = ent->d_name[k];
                    k++;
                }
                ents[count].name[k] = '\0';
                count++;
            }
            closedir(dp);
        }
    } else if (names) {
        char *buf = strdup(names);
        char *save = NULL;
        char *tok = buf ? strtok_r(buf, "|", &save) : NULL;
        while (tok && count < maxn) {
            if (name_ok(tok)) {
                size_t k = 0;
                while (tok[k] && k + 1 < sizeof(ents[count].name)) {
                    ents[count].name[k] = tok[k];
                    k++;
                }
                ents[count].name[k] = '\0';
                count++;
            }
            tok = strtok_r(NULL, "|", &save);
        }
        free(buf);
    }
    app_fs_unlock();
    return count;
}

static esp_err_t zip_get(httpd_req_t *req)
{
    char *query = malloc(1800);
    char dir[160] = "/sdcard";
    char all[8] = {0};
    char *names = malloc(1500);
    zip_ent_t *ents = NULL;
    uint8_t *chunk = NULL;
    int count = 0;
    uint32_t offset = 0;
    uint32_t cd_offset = 0;
    esp_err_t err = ESP_OK;
    if (!query || !names) {
        free(query);
        free(names);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
        return ESP_FAIL;
    }
    names[0] = '\0';
    if (httpd_req_get_url_query_str(req, query, 1800) == ESP_OK) {
        char value[160];
        if (httpd_query_key_value(query, "dir", value, sizeof(value)) == ESP_OK) {
            url_decode(value);
            if (path_ok(value)) {
                snprintf(dir, sizeof(dir), "%s", value);
            }
        }
        httpd_query_key_value(query, "all", all, sizeof(all));
        if (httpd_query_key_value(query, "names", names, 1500) == ESP_OK) {
            url_decode(names);
        } else {
            names[0] = '\0';
        }
    }
    free(query);
    ents = calloc(80, sizeof(zip_ent_t));
    chunk = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ents || !chunk) {
        free(ents);
        free(names);
        free(chunk);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
        return ESP_FAIL;
    }
    app_logger_flush();
    count = collect_zip_names(dir, names, strcmp(all, "1") == 0, ents, 80);
    free(names);
    if (count <= 0) {
        free(ents);
        free(chunk);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no files");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/zip");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"logs.zip\"");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    for (int i = 0; i < count && err == ESP_OK; i++) {
        char full[256];
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

void app_httpd_start(void)
{
    if (s_server) {
        return;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.stack_size = 8192;
    config.max_uri_handlers = 16;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 20;
    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        s_server = NULL;
        return;
    }
    const httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get};
    const httpd_uri_t status = {.uri = "/api/status", .method = HTTP_GET, .handler = status_get};
    const httpd_uri_t files = {.uri = "/api/files", .method = HTTP_GET, .handler = files_get};
    const httpd_uri_t port = {.uri = "/api/port", .method = HTTP_GET, .handler = port_post};
    const httpd_uri_t clock = {.uri = "/api/time", .method = HTTP_GET, .handler = time_get};
    const httpd_uri_t dl = {.uri = "/dl", .method = HTTP_GET, .handler = download_get};
    const httpd_uri_t zip = {.uri = "/api/zip", .method = HTTP_GET, .handler = zip_get};
    const httpd_uri_t live = {.uri = "/api/live", .method = HTTP_GET, .handler = live_get};
    const httpd_uri_t bridge_st = {.uri = "/api/bridge", .method = HTTP_GET, .handler = bridge_status_get};
    const httpd_uri_t bridge_tcp = {.uri = "/api/bridge_tcp", .method = HTTP_GET, .handler = bridge_tcp_post};
    const httpd_uri_t bridge_ble = {.uri = "/api/bridge_ble", .method = HTTP_GET, .handler = bridge_ble_post};
    httpd_register_uri_handler(s_server, &root);
    httpd_register_uri_handler(s_server, &status);
    httpd_register_uri_handler(s_server, &files);
    httpd_register_uri_handler(s_server, &port);
    httpd_register_uri_handler(s_server, &clock);
    httpd_register_uri_handler(s_server, &dl);
    httpd_register_uri_handler(s_server, &zip);
    httpd_register_uri_handler(s_server, &live);
    httpd_register_uri_handler(s_server, &bridge_st);
    httpd_register_uri_handler(s_server, &bridge_tcp);
    httpd_register_uri_handler(s_server, &bridge_ble);
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
