from flask import Flask, request, jsonify, render_template
import sqlite3, os
from datetime import datetime, timezone
app = Flask(
    __name__,
    template_folder='.',
    static_folder='.',
    static_url_path=''
)

DB = os.getenv('DB_PATH', os.path.join(os.path.dirname(__file__), 'leituras.db'))

def connect():
    conn = sqlite3.connect(DB)
    conn.row_factory = sqlite3.Row
    return conn

def init_db():
    with connect() as db:
        db.executescript(open(os.path.join(os.path.dirname(__file__), 'schema.sql'), encoding='utf-8').read())

@app.get('/')
def dashboard():
    return render_template('index.html')

@app.route('/leituras', methods=['GET', 'POST'])
def leituras():
    if request.method == 'GET':
        limit = min(max(request.args.get('limit', 100, type=int), 1), 100)
        with connect() as db:
            rows = db.execute('SELECT * FROM leituras ORDER BY id DESC LIMIT ?', (limit,)).fetchall()
        return jsonify([dict(r) for r in rows])
    payload = request.get_json(silent=True)
    required = ('temperatura_c', 'umidade_pct', 'luminosidade', 'presenca', 'probabilidade_vida')
    if not isinstance(payload, dict) or any(k not in payload for k in required):
        return jsonify(erro='Campos obrigatórios ausentes'), 400
    try:
        temp = float(payload['temperatura_c']); umi = float(payload['umidade_pct'])
        luz = float(payload['luminosidade']); presenca = int(payload['presenca'])
        prob = float(payload['probabilidade_vida'])
        if not (0 <= umi <= 100 and 0 <= prob <= 100 and presenca in (0, 1)):
            raise ValueError('Valores fora dos limites')
        stamp = payload.get('timestamp') or datetime.now(timezone.utc).isoformat()
        with connect() as db:
            cursor = db.execute('INSERT INTO leituras (timestamp,temperatura_c,umidade_pct,luminosidade,presenca,probabilidade_vida) VALUES (?,?,?,?,?,?)', (stamp,temp,umi,luz,presenca,prob))
            row_id = cursor.lastrowid
        return jsonify(id=row_id, mensagem='Leitura registrada'), 201
    except (TypeError, ValueError, OverflowError):
        return jsonify(erro='Dados inválidos'), 400

if __name__ == '__main__':
    init_db()
    app.run(host='0.0.0.0', port=int(os.getenv('PORT', 5000)), debug=False)
