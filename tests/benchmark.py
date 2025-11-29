from locust import HttpUser, task, between
import random
import string

# Simulating a "Viral Document" that everyone is reading
# This represents the "System Prompt" or "Context"
VIRAL_CONTEXT = "The Ranvier node is a gap in the myelin sheath of a nerve, between adjacent Schwann cells." * 10

class AIUser(HttpUser):
    wait_time = between(0.1, 0.5) # Fast users

    @task(4) # 4x more likely to be a "Viral" request
    def viral_query(self):
        # Users asking different questions about the SAME context
        # This SHOULD hit the cache
        question = ''.join(random.choices(string.ascii_lowercase, k=10))
        payload = {
            "model": "gpt2",
            "messages": [
                {"role": "system", "content": VIRAL_CONTEXT},
                {"role": "user", "content": f"Explain this: {question}"}
            ]
        }
        # We tag this request as "viral" to track it separately in logs
        self.client.post("/v1/chat/completions", json=payload, name="Viral_Context_Hit")

    @task(1)
    def random_noise(self):
        # Totally random requests (Short)
        # These should result in Cache Misses (and random routing)
        noise = ''.join(random.choices(string.ascii_lowercase, k=20))
        payload = {
            "model": "gpt2",
            "messages": [{"role": "user", "content": noise}]
        }
        self.client.post("/v1/chat/completions", json=payload, name="Random_Noise_Miss")
