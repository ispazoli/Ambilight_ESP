/* AMBILIGHT BRIDGE — PREVIEW BOOTSTRAP v2
 * Standalone preview only. Never connects to ESP32 and never asks for credentials.
 */
(function(){
  "use strict";
  window.__AMBILIGHT_PREVIEW__=true;
  try{
    localStorage.removeItem("ab_auth");
    localStorage.setItem("ab_ip","preview");
  }catch(e){}
  function boot(){
    const modal=document.getElementById("connectModal");
    if(modal) modal.style.display="none";
    try{
      if(typeof setESPAddress==="function") setESPAddress("preview");
      else { window.espHost="preview"; window.espPort=8080; window.espIP="preview"; }
    }catch(e){}
    try{ if(typeof loadAll==="function") loadAll(); }catch(e){}
    if(!document.getElementById("previewBadge")){
      const badge=document.createElement("div");
      badge.id="previewBadge";
      badge.textContent="UI PREVIEW · ESP32 NÉLKÜL";
      badge.style.cssText="position:fixed;right:14px;bottom:14px;z-index:10000;padding:8px 12px;border-radius:999px;background:#1d1d1f;color:#fff;font:600 10px system-ui;letter-spacing:.5px;box-shadow:0 4px 18px rgba(0,0,0,.18)";
      document.body.appendChild(badge);
    }
  }
  if(document.readyState==="loading") document.addEventListener("DOMContentLoaded",boot,{once:true});
  else boot();
})();