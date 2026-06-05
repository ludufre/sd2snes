const $ = s => document.querySelector(s);
let cwd = "/";

/* ---- i18n (language comes from the sd2snes: 0=EN, 1=PT-BR, 2=ES) ---- */
const LANGS = ["en", "pt", "es"];
let lang = "en";
const I18N = {
  en: {
    refresh:"Refresh", newFolder:"New folder", upload:"Upload", wifi:"WiFi",
    dropHint:"Drag files here to upload", colName:"Name", colSize:"Size",
    loading:"Loading...", empty:"(empty folder)", items:"{0} item(s)",
    listErr:"List error: {0}", download:"download", rename:"rename", del:"delete",
    cfmDelete:'Delete "{0}"?', delFail:"Delete failed (status {0})",
    pNewName:"New name:", renFail:"Rename failed (status {0})",
    pNewFolder:"New folder name:", mkdirFail:"Create folder failed (status {0})",
    downloading:"Downloading", uploading:"Uploading",
    incomplete:"Incomplete: {0} of {1} - try again",
    saved:"saved", sent:"sent", done:"done", fail:"Failed: {0}", upFail:"Upload failed: {0}",
    wifiTitle:"WiFi network", forget:"Forget", scan:"Scan", close:"Close", cancel:"Cancel",
    connect:"Connect", password:"Password",
    connecting:"Connecting to {0}... (if the sd2snes AP drops, reconnect)",
    connected:"Connected: {0} ({1})", notConn:"Not connected - sd2snes AP only",
    stUnavail:"status unavailable", scanning:"Scanning networks...",
    noNets:"No networks found", scanFail:"Scan failed",
    connFail:"Could not connect to {0} - check password/signal",
    opening:"Connected: {0} ({1}) - opening {2}...",
    cfmForget:"Forget the saved network?", mcuNo:"MCU not responding"
  },
  pt: {
    refresh:"Atualizar", newFolder:"Nova pasta", upload:"Enviar", wifi:"WiFi",
    dropHint:"Arraste arquivos aqui para enviar", colName:"Nome", colSize:"Tamanho",
    loading:"Carregando...", empty:"(pasta vazia)", items:"{0} item(ns)",
    listErr:"Erro ao listar: {0}", download:"baixar", rename:"renomear", del:"apagar",
    cfmDelete:'Apagar "{0}"?', delFail:"Falha ao apagar (status {0})",
    pNewName:"Novo nome:", renFail:"Falha ao renomear (status {0})",
    pNewFolder:"Nome da nova pasta:", mkdirFail:"Falha ao criar pasta (status {0})",
    downloading:"Baixando", uploading:"Enviando",
    incomplete:"Incompleto: {0} de {1} - tente de novo",
    saved:"salvo", sent:"enviado", done:"concluído", fail:"Falha: {0}", upFail:"Falha ao enviar: {0}",
    wifiTitle:"Rede WiFi", forget:"Esquecer", scan:"Procurar", close:"Fechar", cancel:"Cancelar",
    connect:"Conectar", password:"Senha",
    connecting:"Conectando a {0}... (se o AP sd2snes cair, reconecte)",
    connected:"Conectado: {0} ({1})", notConn:"Não conectado - somente o AP sd2snes",
    stUnavail:"status indisponível", scanning:"Procurando redes...",
    noNets:"Nenhuma rede encontrada", scanFail:"Falha ao procurar",
    connFail:"Não conectou a {0} - verifique a senha/sinal",
    opening:"Conectado: {0} ({1}) - abrindo {2}...",
    cfmForget:"Esquecer a rede salva?", mcuNo:"MCU sem resposta"
  },
  es: {
    refresh:"Actualizar", newFolder:"Nueva carpeta", upload:"Subir", wifi:"WiFi",
    dropHint:"Arrastra archivos aquí para subir", colName:"Nombre", colSize:"Tamaño",
    loading:"Cargando...", empty:"(carpeta vacía)", items:"{0} elemento(s)",
    listErr:"Error al listar: {0}", download:"descargar", rename:"renombrar", del:"borrar",
    cfmDelete:'¿Borrar "{0}"?', delFail:"Error al borrar (estado {0})",
    pNewName:"Nuevo nombre:", renFail:"Error al renombrar (estado {0})",
    pNewFolder:"Nombre de la nueva carpeta:", mkdirFail:"Error al crear carpeta (estado {0})",
    downloading:"Descargando", uploading:"Subiendo",
    incomplete:"Incompleto: {0} de {1} - inténtalo de nuevo",
    saved:"guardado", sent:"subido", done:"completado", fail:"Error: {0}", upFail:"Error al subir: {0}",
    wifiTitle:"Red WiFi", forget:"Olvidar", scan:"Buscar", close:"Cerrar", cancel:"Cancelar",
    connect:"Conectar", password:"Contraseña",
    connecting:"Conectando a {0}... (si cae el AP sd2snes, reconéctate)",
    connected:"Conectado: {0} ({1})", notConn:"No conectado - solo el AP sd2snes",
    stUnavail:"estado no disponible", scanning:"Buscando redes...",
    noNets:"No se encontraron redes", scanFail:"Error al buscar",
    connFail:"No se pudo conectar a {0} - revisa contraseña/señal",
    opening:"Conectado: {0} ({1}) - abriendo {2}...",
    cfmForget:"¿Olvidar la red guardada?", mcuNo:"MCU sin respuesta"
  }
};
function t(k){
  let s = (I18N[lang] && I18N[lang][k]) || I18N.en[k] || k;
  for (let i = 1; i < arguments.length; i++) s = s.replace("{" + (i - 1) + "}", arguments[i]);
  return s;
}
function applyI18n(){
  document.documentElement.lang = (lang === "pt") ? "pt-br" : lang;
  $("#b_reload").textContent = "↻ " + t("refresh");
  $("#b_mkdir").textContent  = "📁 " + t("newFolder");
  $("#b_upload").textContent = "⬆ " + t("upload");
  $("#b_wifi").textContent   = "📶 " + t("wifi");
  $("#drop").textContent     = t("dropHint");
  $("#b_cname").textContent  = t("colName");
  $("#b_csize").textContent  = t("colSize");
  $("#b_wtitle").textContent = t("wifiTitle");
  $("#wforget").textContent  = t("forget");
  $("#wscan").textContent    = t("scan");
  $("#b_wclose").textContent = t("close");
}

const status = m => { $("#status").textContent = m || ""; };
function human(n){ if(n<1024)return n+" B"; const u=["KB","MB","GB"]; let i=-1;
  do{ n/=1024; i++; }while(n>=1024 && i<2); return n.toFixed(n<10?1:0)+" "+u[i]; }
function join(dir, name){ return (dir==="/"?"":dir)+"/"+name; }

function crumbs(){
  const c=$("#crumbs"); c.innerHTML="";
  const root=document.createElement("a"); root.textContent="SD"; root.onclick=()=>go("/"); c.appendChild(root);
  let acc="";
  cwd.split("/").filter(Boolean).forEach(p=>{ acc+="/"+p; const seg=acc;
    const s=document.createElement("span"); s.className="sep"; s.textContent="/"; c.appendChild(s);
    const a=document.createElement("a"); a.textContent=p; a.onclick=()=>go(seg); c.appendChild(a); });
}

function parent(p){ p=(p||"/").replace(/\/+$/,""); const i=p.lastIndexOf("/"); return i<=0?"/":p.slice(0,i); }
function rowUp(tb){
  const tr=document.createElement("tr"), nm=document.createElement("td");
  nm.innerHTML='<span class="name dir"><span class="ic">↑</span><span class="lbl">..</span></span>';
  nm.querySelector(".name").onclick=()=>go(parent(cwd));
  const sz=document.createElement("td"); sz.className="size";
  const ac=document.createElement("td"); ac.className="act";
  tr.append(nm,sz,ac); tb.appendChild(tr);
}
const HIDE = new Set([".", "..", ".DS_Store", ".fseventsd", ".Spotlight-V100",
  ".TemporaryItems", ".Trashes", "Thumbs.db", "System Volume Information",
  "$RECYCLE.BIN", "desktop.ini"]);
const hidden = n => HIDE.has(n) || n.startsWith("._");

async function go(path){
  cwd=path||"/"; crumbs(); status(t("loading"));
  const tb=$("#list");
  tb.innerHTML='<tr><td colspan="3" class="empty"><span class="spin"></span>'+t("loading")+'</td></tr>';
  try{
    const r=await fetch("/api/ls?path="+encodeURIComponent(cwd));
    if(!r.ok) throw new Error("HTTP "+r.status);
    const j=await r.json();
    const ents=(j.entries||[]).filter(e=>!hidden(e.name))
      .sort((a,b)=>(b.dir-a.dir)||a.name.localeCompare(b.name,undefined,{numeric:true}));
    tb.innerHTML="";
    if(cwd!=="/") rowUp(tb);
    for(const e of ents){
      const full=join(cwd,e.name);
      const tr=document.createElement("tr");
      const nm=document.createElement("td");
      nm.innerHTML='<span class="name'+(e.dir?' dir':'')+'"><span class="ic">'+(e.dir?'📁':'📄')+'</span><span class="lbl"></span></span>';
      nm.querySelector(".lbl").textContent=e.name;
      if(e.dir) nm.querySelector(".name").onclick=()=>go(full);
      const sz=document.createElement("td"); sz.className="size"; sz.textContent=e.dir?"-":human(e.size);
      const ac=document.createElement("td"); ac.className="act";
      if(!e.dir){ const d=document.createElement("a"); d.textContent=t("download"); d.className="always";
        d.onclick=()=>download(full,e.name,e.size); ac.appendChild(d); }
      const rn=document.createElement("a"); rn.textContent=t("rename"); rn.onclick=()=>rename(full,e.name); ac.appendChild(rn);
      const rm=document.createElement("a"); rm.className="danger"; rm.textContent=t("del"); rm.onclick=()=>del(full,e.name); ac.appendChild(rm);
      tr.append(nm,sz,ac); tb.appendChild(tr);
    }
    if(!ents.length){ const tr=document.createElement("tr");
      tr.innerHTML='<td colspan="3" class="empty">'+t("empty")+'</td>'; tb.appendChild(tr); }
    status(t("items", ents.length));
  }catch(err){ $("#list").innerHTML=""; status(t("listErr", err.message)); }
}
const reload=()=>go(cwd);

async function del(full,name){ if(!confirm(t("cfmDelete",name)))return;
  const j=await(await fetch("/api/rm?path="+encodeURIComponent(full))).json();
  if(!j.ok) status(t("delFail",j.status)); reload(); }
async function rename(full,name){ const nn=prompt(t("pNewName"),name); if(!nn||nn===name)return;
  const j=await(await fetch("/api/mv?from="+encodeURIComponent(full)+"&to="+encodeURIComponent(join(cwd,nn)))).json();
  if(!j.ok) status(t("renFail",j.status)); reload(); }
async function mkdir(){ const nn=prompt(t("pNewFolder")); if(!nn)return;
  const j=await(await fetch("/api/mkdir?path="+encodeURIComponent(join(cwd,nn)))).json();
  if(!j.ok) status(t("mkdirFail",j.status)); reload(); }

/* ---- progress modal (download + upload, baud-limited) ---- */
let ovCancel=null;
function ovOpen(title,name){ $("#ovTitle").textContent=title; $("#ovName").textContent=name;
  $("#ovErr").textContent=""; $("#ovBar").style.width="0"; $("#ovPct").textContent="0%";
  $("#ovInfo").textContent=""; $("#ovBtn").textContent=t("cancel"); $("#ov").classList.add("show"); }
function ovClose(){ $("#ov").classList.remove("show"); ovCancel=null; }
function ovProg(loaded,total,secs){
  const pct = total? Math.min(100, loaded/total*100) : 0;
  $("#ovBar").style.width=pct+"%"; $("#ovPct").textContent=(total?pct.toFixed(0):"-")+"%";
  const sp = secs>0? loaded/secs : 0;
  const eta = (sp>0&&total)? (total-loaded)/sp : 0;
  $("#ovInfo").textContent=human(loaded)+(total?" / "+human(total):"")+
    (sp?"  |  "+human(sp)+"/s":"")+(eta>1?"  |  "+Math.ceil(eta)+"s":"");
}
function ovDone(msg){ $("#ovBar").style.width="100%"; $("#ovPct").textContent="100%";
  $("#ovInfo").textContent=msg||t("done"); $("#ovBtn").textContent=t("close"); }
function ovError(msg){ $("#ovErr").textContent=msg; $("#ovBtn").textContent=t("close"); }
$("#ovBtn").onclick=()=>{ if(ovCancel){ ovCancel(); } else ovClose(); };

async function download(full,name,size){
  ovOpen(t("downloading"),name);
  const ctrl=new AbortController(); ovCancel=()=>{ ctrl.abort(); ovClose(); };
  try{
    const resp=await fetch("/api/dl?path="+encodeURIComponent(full),{signal:ctrl.signal});
    if(!resp.ok) throw new Error("HTTP "+resp.status);
    const total=size||+resp.headers.get("Content-Length")||0;
    const reader=resp.body.getReader(); const chunks=[]; let got=0; const t0=performance.now();
    for(;;){ const{done,value}=await reader.read(); if(done)break;
      chunks.push(value); got+=value.length; ovProg(got,total,(performance.now()-t0)/1000); }
    if(total && got<total){ ovCancel=null; ovError(t("incomplete",human(got),human(total))); return; }
    const url=URL.createObjectURL(new Blob(chunks));
    const a=document.createElement("a"); a.href=url; a.download=name; document.body.appendChild(a); a.click(); a.remove();
    URL.revokeObjectURL(url); ovCancel=null; ovDone(t("saved"));
  }catch(err){ if(err.name==="AbortError")return; ovCancel=null; ovError(t("fail",err.message)); }
}

function uploadOne(file){ return new Promise((res,rej)=>{
  const xhr=new XMLHttpRequest(); ovCancel=()=>{ xhr.abort(); ovClose(); };
  const t0=performance.now();
  xhr.open("POST","/api/up?path="+encodeURIComponent(join(cwd,file.name)));
  xhr.upload.onprogress=e=>{ if(e.lengthComputable) ovProg(e.loaded,e.total,(performance.now()-t0)/1000); };
  xhr.onload=()=>{ let ok=false,st=0; try{const j=JSON.parse(xhr.responseText); ok=j.ok; st=j.status;}catch{ ok=xhr.status<300; }
    ok?res():rej(new Error("status "+st)); };
  xhr.onerror=()=>rej(new Error("net")); xhr.onabort=()=>rej(new Error("__abort"));
  xhr.send(file);
}); }

async function upload(files){
  for(const f of files){
    ovOpen(t("uploading"),f.name);
    try{ await uploadOne(f); }
    catch(err){ if(err.message==="__abort")return; ovCancel=null; ovError(t("upFail",err.message)); return; }
  }
  ovCancel=null; ovDone(t("sent")); reload();
}

$("#file").onchange=e=>{ if(e.target.files.length) upload(e.target.files); e.target.value=""; };
const drop=$("#drop");
["dragenter","dragover"].forEach(ev=>drop.addEventListener(ev,e=>{e.preventDefault();drop.classList.add("over");}));
["dragleave","drop"].forEach(ev=>drop.addEventListener(ev,e=>{e.preventDefault();drop.classList.remove("over");}));
drop.addEventListener("drop",e=>{ if(e.dataTransfer.files.length) upload(e.dataTransfer.files); });

/* ---- WiFi ---- */
const wov=$("#wov");
function wclose(){ wov.classList.remove("show"); }
async function wifiOpen(){ wov.classList.add("show"); await wifiStatus(); wifiScan(); }
async function wifiStatus(){
  try{ const s=await(await fetch("/api/wifi/status")).json();
    $("#wstat").textContent = s.connected
      ? t("connected", s.ssid, s.ip+(s.rssi?", "+s.rssi+" dBm":""))
      : t("notConn");
    $("#wforget").style.display = s.connected? "" : "none";
  }catch{ $("#wstat").textContent=t("stUnavail"); }
}
function bars(rssi){ const p=Math.max(0,Math.min(100,2*(rssi+100)));
  return p>=75?"▂▄▆█":p>=50?"▂▄▆":p>=25?"▂▄":"▂"; }
async function wifiScan(){
  $("#wscan").disabled=true; $("#wlist").innerHTML='<div class="wmsg">'+t("scanning")+'</div>';
  try{
    const aps=await(await fetch("/api/wifi/scan")).json();
    const el=$("#wlist"); el.innerHTML="";
    if(!aps.length){ el.innerHTML='<div class="wmsg">'+t("noNets")+'</div>'; }
    aps.forEach(a=>{
      const row=document.createElement("div"); row.className="wrow";
      row.innerHTML='<span class="wsig"></span><span class="wssid"></span><span class="wlock">'+(a.enc?"🔒":"")+'</span>';
      row.querySelector(".wsig").textContent=bars(a.rssi);
      row.querySelector(".wssid").textContent=a.ssid;
      row.onclick=()=>wifiPick(a,row);
      el.appendChild(row);
    });
  }catch{ $("#wlist").innerHTML='<div class="wmsg">'+t("scanFail")+'</div>'; }
  $("#wscan").disabled=false;
}
function wifiPick(a,row){
  document.querySelectorAll(".wform").forEach(f=>f.remove());
  if(!a.enc){ wifiDo(a.ssid,""); return; }
  const f=document.createElement("div"); f.className="wform";
  const inp=document.createElement("input"); inp.type="password"; inp.placeholder=t("password");
  const btn=document.createElement("button"); btn.textContent=t("connect");
  f.append(inp,btn);
  btn.onclick=()=>wifiDo(a.ssid,inp.value);
  inp.onkeydown=e=>{ if(e.key==="Enter") wifiDo(a.ssid,inp.value); };
  row.after(f); inp.focus();
}
async function wifiDo(ssid,pass){
  document.querySelectorAll(".wform").forEach(f=>f.remove());
  $("#wstat").textContent=t("connecting",ssid);
  try{ await fetch("/api/wifi/connect?ssid="+encodeURIComponent(ssid)+"&pass="+encodeURIComponent(pass)); }catch{}
  for(let i=0;i<12;i++){ await new Promise(r=>setTimeout(r,1000));
    try{ const s=await(await fetch("/api/wifi/status")).json();
      if(s.connected && s.ip){
        $("#wforget").style.display="";
        $("#wstat").textContent=t("opening",s.ssid,s.ip,s.ip);
        setTimeout(()=>{ location.href="http://"+s.ip+"/"; }, 1500);
        return;
      }
    }catch{}
  }
  $("#wstat").textContent=t("connFail",ssid);
}
async function wifiForget(){ if(!confirm(t("cfmForget")))return; try{ await fetch("/api/wifi/forget"); }catch{} wifiStatus(); }

/* block zoom (pinch/double-tap), including iOS which ignores user-scalable=no */
["gesturestart","gesturechange","gestureend"].forEach(ev=>document.addEventListener(ev,e=>e.preventDefault()));
let _lt=0; document.addEventListener("touchend",e=>{ const n=Date.now(); if(n-_lt<=300) e.preventDefault(); _lt=n; }, {passive:false});

applyI18n();   /* English defaults until the sd2snes language arrives */
(async()=>{ try{ const j=await(await fetch("/api/ping")).json();
  if(j.ok){ lang = LANGS[j.lang] || "en"; applyI18n(); $("#dev").textContent=j.name+" | proto v"+j.ver; }
  else $("#dev").textContent=t("mcuNo");
}catch{ $("#dev").textContent=t("mcuNo"); } go("/"); })();
