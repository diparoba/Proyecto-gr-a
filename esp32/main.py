import uasyncio as asyncio
import usocket as socket
from machine import UART, Pin

# Configuración de hardware
LED_PIN = 2       # GPIO 2 para LED de status
BAUDRATE = 9600   # Velocidad de comunicación UART (debe coincidir con SoftwareSerial del Nano)

# Inicializar UART y LED
# RX del ESP32 es GPIO 16 (conecta al TX del SoftwareSerial de Arduino, pin 13)
# TX del ESP32 es GPIO 17 (conecta al RX del SoftwareSerial de Arduino, pin 10)
uart = UART(2, tx=Pin(17), rx=Pin(16), baudrate=BAUDRATE)
led = Pin(LED_PIN, Pin.OUT)
led.value(0)  # Apagar LED al inicio

# Búfer circular para logs de depuración (últimas 50 líneas)
logs = []
MAX_LOGS = 50

def get_html_template():
    """Retorna la plantilla HTML de estilo terminal retro."""
    return """<!DOCTYPE html>
<html lang="es">
<head>
    <meta charset="UTF-8">
    <title>Depurador Inalámbrico - Grúa Torre</title>
    <meta http-equiv="refresh" content="2">
    <style>
        body {
            background-color: #0b0f19;
            color: #00ff66;
            font-family: 'Courier New', Courier, monospace;
            padding: 20px;
            font-size: 14px;
            margin: 0;
            display: flex;
            justify-content: center;
        }
        .container {
            width: 100%;
            max-width: 800px;
        }
        .console {
            border: 1px solid rgba(0, 255, 102, 0.3);
            padding: 20px;
            background-color: #05070d;
            min-height: 450px;
            max-height: 80vh;
            overflow-y: auto;
            box-shadow: 0 10px 30px rgba(0, 255, 102, 0.05);
            border-radius: 12px;
        }
        h1 {
            font-size: 18px;
            margin-top: 0;
            border-bottom: 1px solid rgba(0, 255, 102, 0.3);
            padding-bottom: 12px;
            color: #ffffff;
            letter-spacing: 1px;
            text-transform: uppercase;
        }
        .line {
            margin: 6px 0;
            line-height: 1.5;
            word-break: break-all;
        }
        .line::before {
            content: "$ ";
            color: #38bdf8;
        }
        .footer-text {
            margin-top: 15px;
            font-size: 11px;
            color: #64748b;
            text-align: right;
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="console">
            <h1>Consola de Depuración Inalámbrica</h1>
            {log_content}
        </div>
        <div class="footer-text">
            Auto-refresco cada 2 segundos. Últimos 50 logs en RAM.
        </div>
    </div>
</body>
</html>"""

async def uart_reader_task():
    """Lee continuamente los logs desde la conexión serial de Arduino Nano."""
    global logs
    print("Tarea del lector UART iniciada...")
    buffer = b""
    while True:
        try:
            await asyncio.sleep_ms(30)
            if uart.any():
                chunk = uart.read()
                if chunk:
                    buffer += chunk
                    # Evitar crecimiento indefinido del buffer si no hay saltos de línea
                    if len(buffer) > 1024:
                        buffer = buffer[-1024:]
                    while b"\n" in buffer:
                        line, buffer = buffer.split(b"\n", 1)
                        try:
                            # Decodificar y limpiar la traza
                            s = line.decode('utf-8').strip()
                            if s:
                                logs.append(s)
                                if len(logs) > MAX_LOGS:
                                    logs.pop(0)
                                print(f"[UART LOG] {s}")
                        except Exception:
                            pass
        except Exception as e:
            print("Error en uart_reader_task:", e)

async def handle_client(reader, writer):
    """Maneja las solicitudes HTTP entrantes y entrega la consola de logs."""
    try:
        request = await reader.read(1024)
        request_str = request.decode('utf-8')

        if request_str.startswith('GET / '):
            # Renderizar logs en orden cronológico inverso (más recientes arriba)
            log_lines = ""
            for log in reversed(logs):
                log_lines += f'<div class="line">{log}</div>'
            
            if not log_lines:
                log_lines = '<div class="line" style="color: #475569;">Esperando logs de depuración del Arduino Nano...</div>'
            
            html = get_html_template().replace('{log_content}', log_lines)
            response = f"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n{html}"
        else:
            response = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n"

        await writer.awrite(response.encode('utf-8'))
    except Exception as e:
        print(f"Error en handle_client: {e}")
    finally:
        await writer.aclose()

async def main():
    """Inicia el servidor HTTP de depuración."""
    try:
        asyncio.create_task(uart_reader_task())
    except Exception:
        asyncio.ensure_future(uart_reader_task())

    server = await asyncio.start_server(handle_client, '0.0.0.0', 80)
    print("Servidor web asíncrono de depuración iniciado en el puerto 80")
    led.value(1)  # Encender LED para indicar que el sistema está listo
    await server.wait_closed()

# Iniciar servidor
try:
    asyncio.run(main())
except KeyboardInterrupt:
    print("\nServidor web detenido.")