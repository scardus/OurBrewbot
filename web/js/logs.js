'use strict';

// Append a log entry while retaining at most two thousand entries in the browser.
function addLog(text,level='info',timestamp='') {
  logLines.push([timestamp,level,text]);
  if (logLines.length>2000) logLines.splice(0,logLines.length-2000);
}

// Apply log search and level filters, following new entries only when already at the end.
function renderLogs() {
  const q=$('#log-search').value.toLowerCase(),level=$('#log-filter').value;
  const node=$('#log-lines'),follow=node.scrollHeight-node.scrollTop-node.clientHeight<30;
  const filtered=logLines.filter(l=>(level==='all'||l[1]===level)&&l[2].toLowerCase().includes(q));
  node.innerHTML=filtered.map(l=>`<div class="log-line ${l[1]}"><time>${esc(l[0])}</time>${esc(l[2])}</div>`).join('') || '<div class="empty-state">No matching messages.</div>';
  $('.log-count').textContent=logLines.length;
  if (follow) node.scrollTop=node.scrollHeight;
}

let logBusy=false;

// Read incremental log entries and recover the cursor after a controller restart or rejection.
async function pollLogs() {
  if (logBusy || state.logPaused || saving || document.hidden || !state.ready) return;
  logBusy=true;
  try {
    const reply=await request('/logs?boot='+encodeURIComponent(logBoot)+'&after='+logAfter,{text:true},true);
    const boot=reply.headers.get('X-Log-Boot')||'';
    if (logBoot && logBoot!==boot) addLog('Controller restarted. Log cursor reset.','warning');
    const lost=finite(reply.headers.get('X-Log-Lost'));
    if (lost) addLog(lost+' controller log lines were missed.','warning');
    logBoot=boot;
    logAfter=finite(reply.headers.get('X-Log-Next'),logAfter);
    logMore=reply.headers.get('X-Log-More')==='1';
    for (const line of reply.data.split('\n')) {
      const match=/^(\d)\s+(\S+)\s+(.*)$/.exec(line);
      if (match) addLog(match[3],+match[1]<=3?'error':+match[1]===4?'warning':'info',match[2]);
    }
    renderLogs();
    $('.log-bar-status').textContent='Controller stream';
  } catch (error) {
    // A rejected cursor will never recover by sending it again.
    if (error.httpStatus===400) { logBoot=''; logAfter=0; logMore=false; }
    $('.log-bar-status').textContent=error.httpStatus===400?'Log cursor reset · reconnecting':'Log connection lost · retrying';
  }
  finally { logBusy=false; lastLogPoll=Date.now(); }
}
