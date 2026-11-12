"""Distinguish provider trade timestamps from later observed volume activity."""
import argparse
from test_confirmations import run_browser

HARNESS=r'''
window.EventSource=class {addEventListener(){} close(){}};
const data={symbol:'TANH',timezone:'America/New_York',quote_state:'live',order_feed:'live',
  last_trade:{time:Date.parse('2026-11-11T22:59:49Z')/1000,price:36.15,quantity:243},
  last_volume_activity:{interval_start:Date.parse('2026-11-12T11:55:36Z')/1000,observed_at:Date.parse('2026-11-12T11:55:38Z')/1000,quantity:2},
  volume_newer_than_reported_trade:true,windows:[],generated_at:Date.parse('2026-11-12T12:00:00Z')/1000};
window.fetch=async url=>{
 const path=new URL(url,location.origin).pathname;
 const fixtures={'/api/i18n':{md_last_trade_time:'Provider-reported trade time',md_latest_volume_activity:'Latest volume activity observed',md_activity_shares:'shares added',md_trade_time_lags_volume:'Exact execution time was not supplied.'},
 '/api/bootstrap':{user:'lmh',symbols:['TANH'],market_data_symbols:['TANH'],accounts:[],can_access_market_data:true,can_access_admin_control:true},
 '/api/quotes':{rows:{}},'/api/accounts/view':{accounts:[],portfolio:{}},'/api/algo':{active:false,fast:[],smart:{parents:[]},buying:{parents:[]}},'/api/health':{feeds:{}},'/api/market-data':data};
 if(!(path in fixtures))throw Error('Unexpected request '+path);
 return {ok:true,status:200,json:async()=>structuredClone(fixtures[path])};
};
window.addEventListener('load',async()=>{
 const result=document.createElement('pre');result.id='confirmation-test-result';document.body.append(result);
 try{
  for(let i=0;i<100 && !document.querySelector('#md-trade-time-note')?.textContent;i++)await new Promise(r=>setTimeout(r,20));
  const text=document.querySelector('#md-trade').textContent;
  if(!text.includes('Latest volume activity observed')||!text.includes('06:55:38'))throw Error('Later volume observation missing');
  if(!text.includes('Provider-reported trade time')||!text.includes('17:59:49'))throw Error('Provider time was fabricated or omitted');
  if(document.querySelector('#md-trade-time-note').hidden)throw Error('Timestamp mismatch explanation hidden');
  if(document.documentElement.scrollWidth>innerWidth+1)throw Error('Mobile layout overflows');
  result.textContent='PASS: provider time and later volume observation remain distinct';
 }catch(e){result.textContent='FAIL: '+e.message;}
});
'''
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--browser',required=True);args=p.parse_args()
    raise SystemExit(run_browser(args.browser,HARNESS,path='/market-data',width=390))
