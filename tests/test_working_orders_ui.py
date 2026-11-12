"""Exercise the working-orders tab with offline fixtures and exact cancellation."""
import argparse
from test_confirmations import run_browser

HARNESS=r'''
window.EventSource=class {addEventListener(){} close(){}};
const requests=[];
const orders=[
 {account_id:'BOSCO-WXL',symbol:'TANH',order_id:'manual-1',origin:'manual_external',can_cancel:true,status:'NEW',quantity:100,filled_quantity:0,limit_price:39.5},
 {account_id:'BITGET-LYJ',symbol:'POM',order_id:'algo-2',origin:'algo',can_cancel:false,status:'NEW',session_label:'POM-G-33',quantity:100,filled_quantity:0,limit_price:12},
 {account_id:'STAR-DONG',symbol:'TANH',order_id:'ambiguous-3',origin:'unknown',can_cancel:false,status:'NEW',quantity:100,filled_quantity:0,limit_price:39.5}];
window.fetch=async (url,options={})=>{
 const path=new URL(url,location.origin).pathname;
 if(path==='/api/orders/cancel'){
   requests.push(JSON.parse(options.body));orders[0].can_cancel=false;orders[0].cancel_state='queued';
   return {ok:true,status:200,json:async()=>({ok:true,message:'Cancellation queued'})};
 }
 const fixtures={'/api/i18n':{wo_title:'Working orders',wo_manual:'Manual orders',wo_algo:'Algo orders',wo_unknown:'Unresolved ownership',wo_cancel:'Cancel manual order',wo_confirm:'Cancel this manual order?',confirm_action:'Confirm'},
 '/api/bootstrap':{user:'lmh',symbols:['TANH','POM'],accounts:[],can_access_market_data:true,can_access_admin_control:true},
 '/api/quotes':{rows:{}},'/api/accounts/view':{accounts:[],portfolio:{}},'/api/algo':{active:false,fast:[],smart:{parents:[]},buying:{parents:[]}},'/api/health':{feeds:{}},
 '/api/working-orders':{orders,feed:{state:'live'}}};
 if(!(path in fixtures))throw Error('Unexpected request '+path);
 return {ok:true,status:200,json:async()=>structuredClone(fixtures[path])};
};
window.addEventListener('load',async()=>{
 const result=document.createElement('pre');result.id='confirmation-test-result';document.body.append(result);
 const until=async test=>{for(let i=0;i<100;i++){if(test())return;await new Promise(r=>setTimeout(r,20));}throw Error('Timed out');};
 try{
  await until(()=>document.querySelector('#wo-manual button'));
  if(!document.querySelector('#wo-algo').textContent.includes('POM-G-33'))throw Error('Session ID missing');
  if(document.querySelector('#wo-algo button')||document.querySelector('#wo-unknown button'))throw Error('Protected order has cancellation button');
  if(!document.querySelector('#wo-manual').textContent.includes('manual-1'))throw Error('Manual order missing');
  if(/participation|参与率|成交量参与/i.test(document.querySelector('main').textContent))throw Error('Proprietary participation shown');
  document.querySelector('#wo-manual button').click();
  await until(()=>document.querySelector('.modal-actions .red'));
  if(requests.length)throw Error('Cancellation bypassed confirmation');
  document.querySelector('.modal-actions .red').click();
  await until(()=>requests.length===1 && document.querySelector('#wo-manual button')?.disabled);
  if(JSON.stringify(requests[0])!==JSON.stringify({account_ids:['BOSCO-WXL'],order_id:'manual-1'}))throw Error('Wrong cancellation scope');
  if(!document.querySelector('#wo-manual').textContent.includes('manual-1'))throw Error('Pending cancellation hidden prematurely');
  if(document.documentElement.scrollWidth>innerWidth+1)throw Error('Mobile viewport overflows');
  result.textContent='PASS: separate tables, session ID, exact manual cancellation, pending visibility, mobile layout';
 }catch(e){result.textContent='FAIL: '+e.message;}
});
'''

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--browser',required=True);args=p.parse_args()
    raise SystemExit(run_browser(args.browser,HARNESS,path='/working-orders',width=390))
