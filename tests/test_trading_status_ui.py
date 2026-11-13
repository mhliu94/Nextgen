"""Unknown and stale accounts stay disabled, including when a quiet feed ages out."""
import argparse
from test_confirmations import run_browser

HARNESS=r'''
window.requestAnimationFrame=fn=>setTimeout(()=>fn(performance.now()),0);
const listeners={},requests=[];window.EventSource=class{addEventListener(k,f){listeners[k]=f;}close(){}};
const base=Date.now();let now=base;Date.now=()=>now;
const stamp=age=>new Date(base-age).toISOString();
const accounts=[{account_id:'ON',trading_enabled:true,ts:stamp(0)},
 {account_id:'OFF',trading_enabled:false,ts:stamp(0)},
 {account_id:'NOFLAG',ts:stamp(0)},{account_id:'STALE',trading_enabled:true,ts:stamp(300001)},
 {account_id:'INVALID',trading_enabled:false,ts:'bad'},{account_id:'FUTURE',trading_enabled:true,ts:stamp(-60000)},
 {account_id:'META',trading_enabled:true,ts:stamp(0)},{account_id:'EXPIRES',trading_enabled:false,ts:stamp(299500)}];
const metas=['ON','OFF','UNKNOWN','NOFLAG','STALE','INVALID','FUTURE','META','EXPIRES'].map((id,i)=>({id,num_id:i+1,broker:'Test',stale:id==='META'}));
accounts.forEach(a=>{a.positions=[];a.cash=1000;});
window.fetch=async(url,options={})=>{
 const path=new URL(url,location.origin).pathname;
 if(options.method==='POST'){requests.push(JSON.parse(options.body));return {ok:true,status:200,json:async()=>({ok:true})};}
 const fixtures={'/api/i18n':{trading_stale:'Stale / unreachable',trading_unknown:'Unknown'},
 '/api/bootstrap':{user:'lmh',can_manage_trading:true,symbols:['TANH'],accounts:metas,limit_share_shortcuts:[100],through_market_choices:[1]},
 '/api/accounts/view':{accounts,metas,portfolio:{}},'/api/quotes':{rows:{}},
 '/api/algo':{active:false,fast:[],smart:{parents:[]},buying:{parents:[]}},'/api/health':{feeds:{}}};
 if(!(path in fixtures))throw Error(path);
 return {ok:true,status:200,json:async()=>structuredClone(fixtures[path])};
};
const until=async test=>{for(let i=0;i<100;i++){if(test())return;await new Promise(r=>setTimeout(r,20));}throw Error('Timed out');};
const check=(value,message)=>{if(!value)throw Error(message);};
const row=id=>document.querySelector('.trading-status-row[data-account-id="'+id+'"]');
const buttons=id=>[...row(id).querySelectorAll('button')];
window.addEventListener('load',async()=>{
 const result=document.createElement('pre');result.id='confirmation-test-result';document.body.append(result);
 try {
  await until(()=>row('UNKNOWN'));
  for(const id of ['UNKNOWN','NOFLAG','STALE','INVALID','FUTURE','META']){
    check(buttons(id).every(b=>b.disabled && b.classList.contains('gray')),'Unreachable controls enabled or colored: '+id);
    buttons(id).forEach(b=>b.click());
  }
  check(requests.length===0,'Disabled control sent a command');
  check(buttons('ON')[0].disabled && !buttons('ON')[1].disabled,'Fresh enabled account controls incorrect');
  check(!buttons('OFF')[0].disabled && buttons('OFF')[1].disabled,'Fresh disabled account controls incorrect');
  check(!buttons('EXPIRES')[0].disabled,'Fresh expiring account disabled early');
  now=base+700;
  await until(()=>buttons('EXPIRES').every(b=>b.disabled));
  check(buttons('EXPIRES').every(b=>b.classList.contains('gray')),'Aged account not gray without new feed messages');
  accounts.find(a=>a.account_id==='EXPIRES').ts=new Date(now).toISOString();
  listeners.accounts({data:JSON.stringify({accounts,metas,portfolio:{}})});
  await until(()=>!buttons('EXPIRES')[0].disabled);
  check(!buttons('EXPIRES')[0].classList.contains('gray'),'Fresh update did not restore controls');
  check(document.documentElement.scrollWidth<=innerWidth+1,'Mobile view overflows');
  document.querySelector('#nav a[href="/account-details"]').click();
  const cardButtons=id=>[...document.querySelectorAll('.account-card[data-account-id="'+id+'"] [data-account-order]')];
  await until(()=>cardButtons('UNKNOWN').length===2);
  for(const id of ['UNKNOWN','STALE','INVALID','FUTURE','META']){
    check(cardButtons(id).every(b=>b.disabled && b.classList.contains('gray')),'Unavailable account order controls enabled: '+id);
    cardButtons(id).forEach(b=>b.click());
  }
  check(!document.querySelector('.modal-backdrop'),'Unavailable account opened an order dialog');
  check(cardButtons('ON').every(b=>!b.disabled),'Fresh account order controls disabled');
  cardButtons('ON')[0].click();
  await until(()=>document.querySelector('.modal-backdrop form'));
  now=base+301000;
  await until(()=>cardButtons('ON').every(b=>b.disabled && b.classList.contains('gray')));
  document.querySelector('.modal-backdrop form').dispatchEvent(new Event('submit',{bubbles:true,cancelable:true}));
  await new Promise(r=>setTimeout(r,20));
  check(requests.length===0,'Stale open dialog submitted an order');
  document.querySelector('.modal-close').click();
  accounts.find(a=>a.account_id==='ON').ts=new Date(now).toISOString();
  listeners.accounts({data:JSON.stringify({accounts,metas,portfolio:{}})});
  await until(()=>cardButtons('ON').every(b=>!b.disabled));
  check(cardButtons('ON').every(b=>!b.classList.contains('gray')),'Fresh account order controls stayed gray');
  result.textContent='PASS: status and Market/Limit controls disable on missing/stale data, recover on fresh data, and reject stale dialogs';
 }catch(e){result.textContent='FAIL: '+e.message;}
});
'''
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--browser',required=True);args=p.parse_args()
    raise SystemExit(run_browser(args.browser,HARNESS,path='/trading-status',width=390))
