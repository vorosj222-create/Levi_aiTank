"""
Lánctalpas robot – radarkép a PC-n (3. lépés)

Az ESP a servo pásztázása közben méri a távolságot, és UDP-n küldi:
    "R <szög> <távolság_cm>"   (-1 = nincs visszhang)

Telepítés:  pip install pygame
Indítás:    python radar.py

Billentyűk:  C = kép törlése,  M = tükrözés be/ki,  Esc = kilépés
"""

import math
import socket
import time

import pygame

# ---------- Beállítások ----------
ESP_IP = "192.168.1.200"
ESP_PORT = 4210
LOCAL_PORT = 4211

MAX_RANGE_CM = 70         # a radar legkülső köre, ennél messzebbi méréseket eldob
RING_STEP_CM = 10         # ilyen távolságonként rajzol kört
FADE_SEC = 2.5            # ennyi idő alatt halványul el egy mérés
ANGLE_BIN = 3             # ilyen széles szögsávokba gyűjti a méréseket (fok)
MIRROR = False            # ha a radarkép tükrözve van a valósághoz képest: True (vagy M gomb)

W, H = 1000, 620
CX, CY = W // 2, H - 60
R_PX = 440

BG = (5, 15, 8)
GREEN = (0, 255, 90)
DIM = (0, 110, 40)
WHITE = (220, 230, 220)
RED = (255, 60, 60)


def polar(angle_deg, dist_cm, mirror):
    """Servo szög + távolság -> képernyő koordináta. 90° = egyenesen előre."""
    a = math.radians(180 - angle_deg if mirror else angle_deg)
    r = dist_cm / MAX_RANGE_CM * R_PX
    return CX + r * math.cos(a), CY - r * math.sin(a)


def draw_grid(screen, small, mirror):
    for cm in range(RING_STEP_CM, MAX_RANGE_CM + 1, RING_STEP_CM):
        r = cm / MAX_RANGE_CM * R_PX
        rect = pygame.Rect(CX - r, CY - r, 2 * r, 2 * r)
        pygame.draw.arc(screen, DIM, rect, 0, math.pi, 1)
        screen.blit(small.render(f"{cm} cm", True, DIM), (CX + r - 22, CY + 6))
    for a in range(0, 181, 30):
        x, y = polar(a, MAX_RANGE_CM, mirror)
        pygame.draw.line(screen, DIM, (CX, CY), (x, y), 1)
        lx, ly = polar(a, MAX_RANGE_CM * 1.07, mirror)
        txt = small.render(f"{a}°", True, DIM)
        screen.blit(txt, txt.get_rect(center=(lx, ly)))
    pygame.draw.rect(screen, WHITE, (CX - 12, CY - 8, 24, 16), 2)  # a robot


def main():
    mirror = MIRROR

    pygame.init()
    screen = pygame.display.set_mode((W, H))
    pygame.display.set_caption("Robot radar")
    font = pygame.font.SysFont(None, 26)
    small = pygame.font.SysFont(None, 20)
    clock = pygame.time.Clock()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("", LOCAL_PORT))
    sock.setblocking(False)

    def send(msg):
        try:
            sock.sendto(msg.encode(), (ESP_IP, ESP_PORT))
        except OSError:
            pass

    points = {}            # szögsáv -> (szög, távolság, időbélyeg)
    sweep_angle = 90
    last_dist = -1
    last_rx = 0.0
    last_hello = 0.0
    rx_count = 0
    rate = 0.0
    rate_t = time.time()

    running = True
    while running:
        now = time.time()

        for ev in pygame.event.get():
            if ev.type == pygame.QUIT:
                running = False
            elif ev.type == pygame.KEYDOWN:
                if ev.key == pygame.K_ESCAPE:
                    running = False
                elif ev.key == pygame.K_c:
                    points.clear()
                elif ev.key == pygame.K_m:
                    mirror = not mirror

        # Másodpercenként "HELLO": ebből tudja az ESP, hova küldjön (újraindulás után is)
        if now - last_hello >= 1.0:
            send("HELLO")
            last_hello = now

        # ---------- Fogadás ----------
        while True:
            try:
                data, _ = sock.recvfrom(256)
            except BlockingIOError:
                break
            except ConnectionResetError:   # Windows UDP sajátosság
                continue
            last_rx = now
            parts = data.decode(errors="ignore").split()
            if len(parts) == 3 and parts[0] == "R":
                try:
                    a, d = int(parts[1]), int(parts[2])
                except ValueError:
                    continue
                if a < 0:
                    continue
                sweep_angle, last_dist = a, d
                points[a // ANGLE_BIN] = (a, d, now)
                rx_count += 1

        if now - rate_t >= 1.0:
            rate = rx_count / (now - rate_t)
            rx_count = 0
            rate_t = now

        # ---------- Rajzolás ----------
        screen.fill(BG)
        draw_grid(screen, small, mirror)

        for key, (a, d, t) in list(points.items()):
            age = now - t
            if age > FADE_SEC:
                del points[key]
                continue
            if d <= 0 or d > MAX_RANGE_CM:
                continue
            f = 1 - age / FADE_SEC
            x, y = polar(a, d, mirror)
            pygame.draw.circle(screen, (int(255 * f), int(255 * f), int(80 * f)), (int(x), int(y)), 6)

        sx, sy = polar(sweep_angle, MAX_RANGE_CM, mirror)
        pygame.draw.line(screen, GREEN, (CX, CY), (sx, sy), 2)

        connected = now - last_rx < 1.5
        in_range = 0 < last_dist <= MAX_RANGE_CM
        hud = [
            (f"Kapcsolat: OK ({rate:.0f} mérés/s)" if connected else "Kapcsolat: nincs adat", GREEN if connected else RED),
            (f"Szög: {sweep_angle}°   Távolság: {last_dist if in_range else '-'} cm", WHITE),
            (f"Tükrözés: {'BE' if mirror else 'KI'}", WHITE),
        ]
        for i, (txt, col) in enumerate(hud):
            screen.blit(font.render(txt, True, col), (15, 12 + i * 26))
        screen.blit(small.render("C: törlés | M: tükrözés | Esc: kilépés", True, DIM), (15, H - 26))

        pygame.display.flip()
        clock.tick(60)

    pygame.quit()


if __name__ == "__main__":
    main()