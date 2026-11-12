"""Algo navigation, exact per-session stops, active filtering and stop-all entitlement."""
import argparse
from test_confirmations import run_browser

HARNESS=r'''
const requests=[],listeners={},errors=[];let stage='initial render';
window.requestAnimationFrame=fn=>setTimeout(()=>fn(performance.now()),0);
window.addEventListener('error',e=>errors.push(e.message));window.addEventListener('unhandledrejection',e=>errors.push(String(e.reason)));
window.EventSource=class{addEventListener(name,fn){listeners[name]=fn;}close(){}};
const fast=(id,mode)=>({session_id:id,display_id:'TANH-'+mode+'-'+id,command:{trading_mode:mode,symbol:'TANH',fast_trading_account_ids:['A']},started_at:new Date().toISOString(),cycles:2,published_commands:1,last_cycle:{reason:'waiting'},stopping:false});
const parent=(id,active,state)=>({parent_id:id,display_id:'POM-G-'+id,symbol:'POM',account_ids:['A'],active,state,quantity:'777777',filled:'20',remaining:'80',reserved:'10',start:new Date().toISOString(),mode:'normal',reason:'waiting for eligible tape volume',market_state:'TRADING',accounts:{A:{quantity:'777777',filled:'20',average_cost:'999999'}},free_credit:'1',child_cap:'100'});
const algo={active:true,fast:[fast('1','E'),fast('2','E'),fast('3','F')],smart:{parents:[parent('4',true,'HOLD'),parent('5',false,'STOPPED')]},buying:{parents:[{...parent('6',true,'HOLD'),display_id:'POM-H-6',quantity:'100'}]}};
window.fetch=async(url,options={})=>{
 const path=new URL(url,location.origin).pathname;
 if(options.method==='POST') {
  requests.push(path);
  if(path==='/api/algo/fast/1/stop')algo.fast=algo.fast.filter(s=>s.session_id!=='1');
  if(path==='/api/smart-selling/4/stop')algo.smart.parents[0].active=false;
  return {ok:true,status:200,json:async()=>({ok:true})};
 }
 const fixtures={'/api/i18n':{nav_algo:'ALGO_LABEL',nav_control:'Trading',smart_state_paused:'Paused',smart_inventory:'Initial inventory',smart_stop:'Stop',algo_confirm_stop_one:'Stop this session?'},
 '/api/bootstrap':{user:'TEST_USER',can_stop_all_algorithms:STOP_ALL,symbols:['TANH'],accounts:[],limit_share_shortcuts:[],through_market_choices:[]},
 '/api/quotes':{rows:{}},'/api/accounts/view':{accounts:[],portfolio:{}},'/api/algo':algo,'/api/health':{feeds:{}}};
 if(!(path in fixtures))throw Error(path);
 return {ok:true,status:200,json:async()=>structuredClone(fixtures[path])};
};
const until=async f=>{for(let i=0;i<100;i++){if(f())return;await new Promise(r=>setTimeout(r,20));}throw Error('Timed out: '+stage+' '+errors.join(';'));};
const check=(value,message)=>{if(!value)throw Error(message);};
window.addEventListener('load',async()=>{
 const result=document.createElement('pre');result.id='confirmation-test-result';document.body.append(result);
 try {
  await until(()=>document.querySelector('[data-stop-session="1"]'));
  const links=[...document.querySelectorAll('#nav a')].map(a=>a.pathname);
  check(links.indexOf('/algo')===links.indexOf('/control-panel')+1,'Algo is not beside Trading');
  check(document.querySelector('#nav a[href="/algo"]').textContent.includes('ALGO_LABEL'),'Wrong Algo label');
  check(!!document.querySelector('#algo-stop-all-form')===STOP_ALL,'Wrong stop-all entitlement');
  check(document.querySelectorAll('[data-stop-session]').length===5,'Not all active E/F/G/H sessions are shown');
  check(document.querySelector('#smart-status').textContent.includes('Paused'),'Paused session hidden');
  check(!document.querySelector('#content').textContent.includes('POM-G-5'),'Concluded session shown');
  check(!document.querySelector('#smart-status').textContent.includes('777,777'),'Initial inventory displayed');
  check(!document.querySelector('#smart-status').textContent.includes('999,999'),'Initial cost displayed');
  document.querySelector('[data-stop-session="1"]').click();
  stage='E confirmation';
  await until(()=>document.querySelector('.modal-actions .red'));
  check(requests.length===0,'Single stop bypassed confirmation');
  document.querySelector('.modal-actions .red').click();
  stage='E stopped';
  await until(()=>!document.querySelector('[data-stop-session="1"]'));
  check(requests[0]==='/api/algo/fast/1/stop','Wrong E session stop request');
  check(document.querySelector('[data-stop-session="2"]'),'Other E session stopped');
  document.querySelector('[data-stop-session="4"]').click();
  stage='G stopped';
  await until(()=>document.querySelector('.modal-actions .red'));document.querySelector('.modal-actions .red').click();
  await until(()=>document.querySelector('[data-algo-mode="G"]').hidden);
  check(requests[1]==='/api/smart-selling/4/stop','Wrong G session stop request');
  check(!document.querySelector('[data-algo-mode="H"]').hidden,'Paused H session hidden');
  listeners.algo({data:JSON.stringify({fast:[],smart:{parents:[]},buying:{parents:[]},active:false})});
  stage='empty state';
  await until(()=>!document.querySelector('#algo-empty').hidden);
  check([...document.querySelectorAll('[data-algo-mode]')].every(s=>s.hidden),'Empty modes shown');
  check(document.documentElement.scrollWidth<=innerWidth+1,'Mobile page overflows');
  document.querySelector('#nav a[href="/control-panel"]').click();
  check(!document.querySelector('#stop_reason') && !document.querySelector('#smart-status'),'Session view or stop-all remains on Trading');
  result.textContent='PASS: Algo tab, all active modes, paused sessions, exact stops, stop-all entitlement, inventory removed';
 }catch(e){result.textContent='FAIL: '+e.message+' requests='+JSON.stringify(requests)+' buttons='+JSON.stringify([...document.querySelectorAll('[data-stop-session]')].map(b=>b.dataset.stopSession));}
});
'''
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--browser',required=True);args=p.parse_args()
    codes=[]
    for user,label in [('lmh','算法')]:
        harness=HARNESS.replace('TEST_USER',user).replace('ALGO_LABEL',label).replace('STOP_ALL','true' if user=='lmh' else 'false')
        codes.append(run_browser(args.browser,harness,path='/algo',width=390))
    raise SystemExit(max(codes))
