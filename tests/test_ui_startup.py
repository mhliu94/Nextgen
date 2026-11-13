"""Startup, partial-read failure and session-expiry regressions in Chromium."""
import argparse
from test_confirmations import run_browser

HARNESS=r'''
const errors=[],counts={},posts=[];let streams=0,recover=false,releaseAccounts;
window.addEventListener('error',e=>errors.push(e.message));
window.addEventListener('unhandledrejection',e=>errors.push(String(e.reason)));
const nativeTimeout=window.setTimeout.bind(window),nativeInterval=window.setInterval.bind(window);
window.setTimeout=(fn,ms,...args)=>nativeTimeout(fn,ms===10000 ? 150 : ms,...args);
window.setInterval=(fn,ms,...args)=>nativeInterval(fn,ms===5000 ? 200 : ms,...args);
window.requestAnimationFrame=fn=>nativeTimeout(fn,0);
window.EventSource=class {constructor(){streams++;}addEventListener(){}close(){}};
const boot={user:'lmh',symbols:['TANH'],accounts:[{id:'A',num_id:1,broker:'Test'}],
 limit_share_shortcuts:[100],through_market_choices:[1],smart_selling_available:true,smart_buying_available:true};
const accounts={metas:boot.accounts,accounts:[{account_id:'A',ts:new Date().toISOString(),cash:1000,positions:[]}],portfolio:{}};
const algo={active:false,fast:[],smart:{parents:[]},buying:{parents:[]},posting:{parents:[]}};
const dictionary={loading:'Loading',no_active_algo:'No active sessions',view_unavailable:'Temporarily unavailable',
 views_unavailable:'Some information unavailable; retrying',request_timeout:'Request timed out'};
const reply=data=>({ok:true,status:200,json:async()=>structuredClone(data)});
const hang=signal=>new Promise((resolve,reject)=>{
 const fail=()=>reject(new DOMException('Aborted','AbortError'));
 if(signal.aborted)fail();else signal.addEventListener('abort',fail,{once:true});
});
window.fetch=async(url,options={})=>{
 const path=new URL(url,location.origin).pathname;counts[path]=(counts[path]||0)+1;
 if(options.method && options.method!=='GET'){posts.push(path);return reply({ok:true});}
 if(path==='/api/i18n')return reply(dictionary);
 if(path==='/api/bootstrap')return reply(boot);
 if(path==='/api/accounts/view'){
   if(SCENARIO==='expired')return new Promise(resolve=>{releaseAccounts=()=>resolve(reply(accounts));});
   return reply(accounts);
 }
 if(path==='/api/quotes')return reply({rows:{TANH:{last:10}}});
 if(path==='/api/algo'){
   if(SCENARIO==='expired')return {ok:false,status:401,json:async()=>({})};
   if(!recover)return hang(options.signal);
   return reply(algo);
 }
 if(path==='/api/health'){
   if(!recover && SCENARIO!=='expired')return {ok:true,status:200,json:()=>hang(options.signal)};
   return reply({feeds:{accounts:{state:'live'},market:{state:'live'}}});
 }
 throw Error('Unexpected path '+path);
};
const pause=()=>new Promise(resolve=>nativeTimeout(resolve,10));
const until=async(test,message)=>{for(let i=0;i<180;i++){if(test())return;await pause();}throw Error(message);};
const check=(condition,message)=>{if(!condition)throw Error(message);};
window.addEventListener('load',async()=>{
 const result=document.createElement('pre');result.id='confirmation-test-result';document.body.append(result);
 try{
   if(SCENARIO==='expired'){
     await until(()=>document.querySelector('#username'),'Expiry did not show login');
     check(document.querySelector('#topbar').hidden,'Authenticated navigation remained visible');
     releaseAccounts();await pause();await pause();
     check(document.querySelector('#username') && !document.querySelector('.account-card'),'Late read restored an expired session');
     check(document.querySelector('#data-banner').hidden,'Expired session shows stale-data banner');
   }else{
     await until(()=>document.querySelector('.account-card'),'Account view waited for stalled algo/health');
     check(streams===1,'Streaming was blocked by startup reads');
     document.querySelector('#nav a[href="/algo"]').click();
     check(document.querySelector('#algo-empty').textContent==='Loading','Unknown algo state shown as empty');
     await until(()=>!document.querySelector('#data-banner').hidden,'Timed-out read was silently swallowed');
     await until(()=>document.querySelector('#algo-empty').textContent==='Temporarily unavailable','Algo failure not visible');
     document.querySelector('#nav a[href="/control-panel"]').click();
     const field=document.querySelector('#limit_price');field.value='7.77';
     recover=true;
     await until(()=>document.querySelector('#data-banner').hidden,'Failed reads did not retry and recover');
     check(counts['/api/algo']>=2 && counts['/api/health']>=2,'Both stalled reads were not retried');
     check(document.querySelector('#limit_price')===field && field.value==='7.77','Background recovery reset the active form');
     document.querySelector('#nav a[href="/algo"]').click();
     check(document.querySelector('#algo-empty').textContent==='No active sessions','Recovered algo state not rendered');
   }
   check(posts.length===0,'Startup or retry sent a mutation');
   check(errors.length===0,errors.join('; '));
   check(document.documentElement.scrollWidth<=innerWidth+1,'Mobile page overflows');
   result.textContent='PASS: '+SCENARIO+' startup, partial results, bounded reads and session-safe recovery';
 }catch(e){result.textContent='FAIL: '+e.message+'; '+errors.join('; ');}
});
'''

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--browser',required=True)
    args=parser.parse_args()
    for scenario in ['stalled','expired']:
        if run_browser(args.browser,'const SCENARIO='+repr(scenario)+';\n'+HARNESS,path='/account-details',width=390):
            raise SystemExit(1)
