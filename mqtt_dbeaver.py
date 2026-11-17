import json
import logging

import paho.mqtt.client as mqtt
import psycopg2

MQTT_HOST = "localhost"          
MQTT_PORT = 1883
MQTT_TOPIC = "porta/principal/acesso/tentativa"

PG_CONFIG = {
    "host": "localhost",
    "port": 5432,
    "dbname": "smart-lock",
    "user": "postgres",
    "password": "postgres123",
}

logging.basicConfig(level=logging.INFO,
                    format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("ponte")

_conn = None

def get_conn():
    """Retorna uma conexão viva com o Postgres (reconecta se caiu)."""
    global _conn
    if _conn is None or _conn.closed:
        _conn = psycopg2.connect(**PG_CONFIG)
        _conn.autocommit = True
    return _conn


def salvar_acesso(uid: str, autorizado: bool) -> None:
    sql = "INSERT INTO acessos (uid, autorizado) VALUES (%s, %s)"
    try:
        with get_conn().cursor() as cur:
            cur.execute(sql, (uid, autorizado))
    except psycopg2.Error:
        # Tenta uma vez de novo com conexão nova
        global _conn
        _conn = None
        with get_conn().cursor() as cur:
            cur.execute(sql, (uid, autorizado))


def on_connect(client, userdata, flags, reason_code, properties=None):
    log.info("Conectado ao broker (%s)", reason_code)
    # Assina aqui dentro para reassinar automaticamente após reconexão
    client.subscribe(MQTT_TOPIC, qos=1)


def on_message(client, userdata, msg):
    try:
        dados = json.loads(msg.payload.decode("utf-8"))
        uid = str(dados["uid"]).strip().upper()
        autorizado = bool(dados["autorizado"])
        salvar_acesso(uid, autorizado)
        log.info("Registrado: %s -> %s", uid,
                 "AUTORIZADO" if autorizado else "NEGADO")
    except Exception:
        log.exception("Falha ao processar mensagem: %r", msg.payload)


def main():
    # client_id fixo + clean_session=False: se o script ficar fora do ar,
    # o Mosquitto guarda as mensagens QoS 1 e entrega quando voltar.
    client = mqtt.Client(
        mqtt.CallbackAPIVersion.VERSION2,
        client_id="ponte-postgres-fechadura",
        clean_session=False,
    )
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(MQTT_HOST, MQTT_PORT, keepalive=60)
    client.loop_forever()  # reconecta sozinho


if __name__ == "__main__":
    main()