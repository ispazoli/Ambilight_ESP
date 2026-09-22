/* AMBILIGHT BRIDGE — PREVIEW BOOTSTRAP
 * Starts the real UI against the local demo API layer.
 */
(function(){
  window.__AMBILIGHT_PREVIEW__ = true;
  function boot(){
    try{
      if(typeof setESPAddress==="function") setESPAddress("preview");
      else { window.espHost="preview"; window.espPort=8080; window.espIP="preview"; }
      const modal=document.getElementById("connectModal");
      if(modal) modal.style.display="none";
      if(typeof loadAll==="function") loadAll();
      const badge=document.createElement("div");
      badge.textContent="UI PREVIEW · ESP32 NÉLKÜL";
      badge.style.cssText="position:fixed;right:14px;bottom:14px;z-index:10000;padding:8px 12px;border-radius:999px;background:#1d1d1f;color:#fff;font:600 10px system-ui;letter-spacing:.5px;box-shadow:0 4px 18px rgba(0,0,0,.18)";
      document.body.appendChild(badge);
    }catch(e){
      const modal=document.getElementById("connectModal");
      if(modal) modal.style.display="none";
    }
  }
  if(document.readyState==="loading") document.addEventListener("DOMContentLoaded",boot);
  else boot();
})();