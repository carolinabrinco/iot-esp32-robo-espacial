CREATE TABLE IF NOT EXISTS leituras (
 id INTEGER PRIMARY KEY AUTOINCREMENT,
 timestamp TEXT NOT NULL,
 temperatura_c REAL NOT NULL,
 umidade_pct REAL NOT NULL,
 luminosidade REAL NOT NULL,
 presenca INTEGER NOT NULL CHECK (presenca IN (0,1)),
 probabilidade_vida REAL NOT NULL CHECK (probabilidade_vida BETWEEN 0 AND 100)
);
