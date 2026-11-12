"""Offline accounts are unavailable in each algo start checklist."""
import argparse
from test_confirmations import HARNESS as BASE, run_browser

HARNESS=BASE[:BASE.index('window.addEventListener("load"')]
HARNESS=HARNESS.replace('accounts: [], can_manage_trading:', '''accounts: [
 {id:"ONLINE",num_id:1,broker:"Test"},{id:"BOSCO-WJY",num_id:29,broker:"Bosco"},{id:"MISSING",num_id:30,broker:"Test"}],
 fast_trading:{book_level_choices:[1],target_percentage_choices:[10],cycle_seconds_choices:[5]},can_manage_trading:''')
HARNESS=HARNESS.replace('"/api/accounts/view": {accounts: [], portfolio: {}}', '''"/api/accounts/view": {accounts: [
 {account_id:"ONLINE",ts:new Date().toISOString(),cash:1000,positions:[]},
 {account_id:"BOSCO-WJY",ts:"2026-01-01T00:00:00Z",cash:1000,positions:[]}],portfolio:{}}''')
HARNESS+=r'''
window.addEventListener("load",async()=>{
 const result=document.createElement('pre');result.id='confirmation-test-result';document.body.append(result);
 try {
  await until(()=>document.querySelector('#algo-submit'),'Algo form missing');
  for(const mode of ['e','f','g','h']) {
    document.querySelector('#algo-mode-'+mode).click();document.querySelector('#algo-submit').click();
    await until(()=>document.querySelector('.modal-backdrop .check-list'),'Checklist missing');
    const rows=[...document.querySelectorAll('.modal-backdrop .check-list input')];
    check(rows.length===3,'Missing account rows');
    check(!rows[0].disabled,'Fresh account blocked');
    check(rows[1].disabled && rows[2].disabled,'Stale or missing account remained selectable');
    document.querySelector('.modal-close').click();
  }
  check(requests.length===0,'Opening checklist submitted a command');
  result.textContent='PASS: E/F/G/H disable stale and missing accounts while keeping fresh accounts selectable';
 }catch(e){result.textContent='FAIL: '+e.message;}
});
'''
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--browser',required=True);args=p.parse_args()
    raise SystemExit(run_browser(args.browser,HARNESS,width=390))
