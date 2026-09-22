/* AMBILIGHT BRIDGE — PREVIEW BOOTSTRAP v3
 * Standalone UI preview. No ESP32, no credentials, no authentication dialog.
 */
(function(){
  "use strict";
  window.__AMBILIGHT_PREVIEW__=true;
  try{
    localStorage.removeItem("ab_auth");
    localStorage.setItem("ab_ip","preview");
  }catch(e){}
  function boot(){
    try{
      if(typeof setESPAddress==="function") setESPAddress("preview");
      else { window.espHost="preview"; window.espPort=8080; window.espIP="preview"; }
    }catch(e){}
    const modal=document.getElementById("connectModal");
    if(modal) modal.remove();
    try{
      if(typeof loadAll==="function") loadAll();
    }catch(e){
      console.warn("[PREVIEW] boot/loadAll:",e);
    }
  }
  if(document.readyState==="loading") document.addEventListener("DOMContentLoaded",boot,{once:true});
  else boot();
})();
