#pragma once

#include <string>

#include "URLWhitelist.h"

namespace PrismaUI::NetworkSandbox {

inline std::string BuildScript()
{
    std::string script = R"js((function(){
'use strict';
var def=Object.defineProperty.bind(Object);
var dead={value:undefined,writable:false,configurable:false,enumerable:true};
var blocked=['fetch','XMLHttpRequest','WebSocket','EventSource','Worker','SharedWorker'];
for(var i=0;i<blocked.length;++i){try{def(window,blocked[i],dead);}catch(e){}}
try{def(navigator,'sendBeacon',{value:function(){return false;},writable:false,configurable:false});}catch(e){}
try{def(navigator,'serviceWorker',dead);}catch(e){}
try{
var meta=document.createElement('meta');
meta.setAttribute('http-equiv','Content-Security-Policy');
meta.setAttribute('content',")js";
    script += URLWhitelist::GenerateCsp();
    script += R"js(");
if(document.head){document.head.insertBefore(meta,document.head.firstChild);}
}catch(e){}
})();)js";
    return script;
}

}
