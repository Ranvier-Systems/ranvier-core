import asyncio
import aiohttp
import os
import signal
import time

# Configuration
URL = "http://localhost:8080/v1/chat/completions"
# Replace with the actual PID of your running Ranvier process
RANVIER_PID = 12345 

async def send_streaming_request(session, req_id):
    payload = {
        "model": "meta-llama/Meta-Llama-3-8B",
        "messages": [{"role": "user", "content": f"Request {req_id}: Tell me a long story."}],
        "stream": True
    }
    
    try:
        start_time = time.time()
        async with session.post(URL, json=payload) as resp:
            if resp.status != 200:
                print(f"❌ Request {req_id} failed with status {resp.status}")
                return False
            
            # Read the stream to simulate a real client
            async for line in resp.content:
                pass 
            
            duration = time.time() - start_time
            print(f"✅ Request {req_id} finished successfully in {duration:.2f}s")
            return True
    except Exception as e:
        print(f"❌ Request {req_id} encountered an error: {e}")
        return False

async def main():
    async with aiohttp.ClientSession() as session:
        # 1. Start a batch of "In-Flight" requests
        print(f"🚀 Launching 10 concurrent requests...")
        tasks = [send_streaming_request(session, i) for i in range(10)]
        
        # 2. Trigger Hot Reload (SIGHUP) midway
        await asyncio.sleep(1) 
        print(f"🔄 Sending SIGHUP (Hot Reload) to PID {RANVIER_PID}...")
        os.kill(RANVIER_PID, signal.SIGHUP)
        
        # 3. Trigger Graceful Shutdown (SIGTERM)
        await asyncio.sleep(2)
        print(f"🛑 Sending SIGTERM (Graceful Shutdown) to PID {RANVIER_PID}...")
        os.kill(RANVIER_PID, signal.SIGTERM)
        
        # 4. Try to send a NEW request (should be rejected with 503)
        await asyncio.sleep(0.5)
        print(f"📡 Attempting a new request during draining...")
        async with session.post(URL, json={"model": "test"}) as resp:
            if resp.status == 503:
                print("✅ Correctly rejected new request with 503 during shutdown.")
            else:
                print(f"⚠️ Unexpected status for new request: {resp.status}")

        # 5. Wait for the original 10 tasks to finish
        results = await asyncio.gather(*tasks)
        success_count = sum(results)
        print(f"\n📊 Final Result: {success_count}/10 in-flight requests survived.")

if __name__ == "__main__":
    asyncio.run(main())
