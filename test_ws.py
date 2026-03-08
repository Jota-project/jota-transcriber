import asyncio
import websockets

async def test_ws():
    print("Connecting...")
    try:
        async with websockets.connect("wss://127.0.0.1:8003") as ws:
            print("Connected! Handshake successful.")
            await asyncio.sleep(5)
            print("Done")
    except Exception as e:
        print(f"Error: {e}")

asyncio.run(test_ws())
