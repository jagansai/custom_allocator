from pathlib import Path

SOH = "\x01"
TARGET = Path(__file__).resolve().parents[1] / "data" / "fix_messages.txt"
TARGET.parent.mkdir(parents=True, exist_ok=True)

symbols = ["INFY.NS", "AAPL", "MSFT", "GOOGL", "TSLA"]
with TARGET.open("w", encoding="ascii", newline="") as handle:
    for i in range(100_000):
        order_id = f"ORD{i + 1:06d}"
        side = "1" if i % 2 == 0 else "2"
        qty = 1000 + (i % 25) * 50
        price = 1.2345 + (i % 200) * 0.01
        symbol = symbols[i % len(symbols)]
        msg = (
            f"8=FIX.4.2{SOH}"
            f"35=D{SOH}"
            f"49=BUY_SIDE{SOH}"
            f"56=EXCHANGE{SOH}"
            f"11={order_id}{SOH}"
            f"54={side}{SOH}"
            f"38={qty}{SOH}"
            f"44={price:.4f}{SOH}"
            f"48={symbol}{SOH}"
            "10=128"
        )
        handle.write(msg + "\n")
