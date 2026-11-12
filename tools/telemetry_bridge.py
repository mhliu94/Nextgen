"""Carry retained order feedback and live market feeds into Finland; never commands."""
import collections
import json
import logging
import pathlib
import signal
import time
from confluent_kafka import Consumer, Producer, TopicPartition, OFFSET_INVALID

TOPICS=['account-details','order-status','price-books','stock-quotes','trading-volume']

def allowed(topic, value, accounts):
    if topic not in TOPICS:return False
    if topic in {'account-details','order-status'}:
        try:return json.loads(value).get('account_id') in accounts
        except (ValueError,AttributeError):return False
    return True

def main():
    logging.basicConfig(level=logging.INFO,format='%(asctime)s %(levelname)s %(message)s')
    cfg=json.loads(pathlib.Path('/etc/ktrader-finland-bridge.json').read_text())
    accounts=set(cfg['accounts'])
    consumer=Consumer({'bootstrap.servers':cfg['source'],'group.id':'ktrader-finland-telemetry-v1',
        'enable.auto.commit':False,'enable.auto.offset.store':False,'auto.offset.reset':'earliest',
        'max.poll.interval.ms':300000})
    producer=Producer({'bootstrap.servers':cfg['destination'],'enable.idempotence':True,'acks':'all',
        'linger.ms':5,'delivery.timeout.ms':30000})
    running=[True]
    for sig in [signal.SIGTERM,signal.SIGINT]:signal.signal(sig,lambda *_:running.__setitem__(0,False))
    def assigned(c,partitions):
        committed=c.committed(partitions,timeout=10)
        timestamps=[TopicPartition(p.topic,p.partition,int((time.time()-300)*1000)) for p in partitions]
        times={(p.topic,p.partition):p.offset for p in c.offsets_for_times(timestamps,timeout=10)}
        for p,saved in zip(partitions,committed):
            low,high=c.get_watermark_offsets(p,timeout=10)
            if saved.offset>=low:p.offset=min(saved.offset,high)
            elif p.topic=='order-status':p.offset=low
            elif p.topic=='account-details':p.offset=max(low,high-3000)
            else:
                offset=times.get((p.topic,p.partition),-1)
                p.offset=max(low,offset if offset>=0 else high)
        c.assign(partitions)
        logging.info('Assigned telemetry partitions=%d',len(partitions))
    consumer.subscribe(TOPICS,on_assign=assigned)
    counts=collections.Counter();last_log=0
    try:
        while running[0]:
            messages=consumer.consume(500,timeout=1)
            offsets={};errors=[]
            for message in messages:
                if message.error():raise RuntimeError('Source Kafka read failed')
                if allowed(message.topic(),message.value(),accounts):
                    def delivered(error,_):
                        if error:errors.append(str(error))
                    while True:
                        try:
                            args={'topic':message.topic(),'partition':message.partition(),
                                'key':message.key(),'value':message.value(),'headers':message.headers(),
                                'on_delivery':delivered}
                            stamp=message.timestamp()[1]
                            if stamp>=0:args['timestamp']=stamp
                            producer.produce(**args);break
                        except BufferError:producer.poll(.1)
                    counts[message.topic()]+=1
                offsets[(message.topic(),message.partition())]=message.offset()+1
            if offsets:
                remaining=producer.flush(35)
                if remaining or errors:raise RuntimeError('Destination acknowledgement failed; source offsets were not committed')
                consumer.commit(offsets=[TopicPartition(t,p,o) for (t,p),o in offsets.items()],asynchronous=False)
            if time.monotonic()-last_log>=30:
                logging.info('Acknowledged telemetry records=%s',dict(counts));last_log=time.monotonic()
    finally:
        producer.flush(5);consumer.close()

if __name__=='__main__':main()
