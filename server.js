// Servidor da estufa: guarda a chave da IA em segredo e conversa com ela.
require("dotenv").config();
const express = require("express");
const path = require("path");

const CHAVE = process.env.OPENROUTER_API_KEY;
const MODELO = process.env.OPENROUTER_MODEL;
// Pode ter vários modelos separados por vírgula: se um falhar, tenta o próximo.
const MODELOS = (MODELO || "").split(",").map((m) => m.trim()).filter(Boolean);
const PORTA = 3000;

if (!CHAVE || !MODELO) {
  console.error("Faltou a chave ou o modelo (arquivo .env ou variáveis do Vercel).");
  if (require.main === module) process.exit(1); // só encerra quando roda no seu computador
}

const app = express();
app.use(express.json({ limit: "20kb" }));
app.use(express.static(path.join(__dirname, "public")));

// Site: usa a senha SENHA_SITE (opcional). ESP32: usa o token TOKEN_ESP32.
const SENHA = process.env.SENHA_SITE;
const TOKEN_ESP32 = process.env.TOKEN_ESP32;
const ROTAS_ESP32 = ["POST /leitura", "GET /config"]; // únicas rotas que o ESP32 pode usar
app.use("/api", (req, res, next) => {
  if (ROTAS_ESP32.includes(req.method + " " + req.path)) {
    if (!TOKEN_ESP32 || req.get("x-token") !== TOKEN_ESP32) {
      return res.status(401).json({ erro: "Token inválido." });
    }
    return next();
  }
  if (SENHA && req.get("x-senha") !== SENHA) {
    return res.status(401).json({ erro: "Senha incorreta ou ausente." });
  }
  next();
});

// Limites absolutos: a IA nunca pode passar disso, mesmo que erre.
const FAIXAS = { temp: [5, 40], ar: [20, 100], solo: [10, 100], ph: [4, 9] };

async function perguntarIA(system, messages, max_tokens, temperature = 0.2) {
  if (!CHAVE || !MODELOS.length) throw new Error("O servidor está sem chave ou modelo configurado.");
  let ultimoErro = "Nenhum modelo respondeu.";
  for (const modelo of MODELOS) {
    try {
      const r = await fetch("https://openrouter.ai/api/v1/chat/completions", {
        method: "POST",
        headers: {
          "content-type": "application/json",
          Authorization: "Bearer " + CHAVE,
        },
        body: JSON.stringify({
          model: modelo,
          max_tokens,
          temperature,
          messages: [{ role: "system", content: system }, ...messages],
        }),
      });
      const d = await r.json();
      if (!r.ok || d.error) {
        const e = d.error || {};
        const detalhe = [e.message, e.metadata?.provider_name, e.metadata?.raw]
          .filter(Boolean)
          .map((x) => (typeof x === "string" ? x : JSON.stringify(x)))
          .join(" | ")
          .slice(0, 300);
        throw new Error(detalhe || "Erro ao falar com a IA");
      }
      const texto = d.choices?.[0]?.message?.content || "";
      if (texto.trim()) return texto;
      throw new Error("resposta vazia");
    } catch (e) {
      ultimoErro = modelo + ": " + e.message;
      console.error("Falhou com o modelo " + ultimoErro);
    }
  }
  throw new Error(ultimoErro);
}

// Confere se os limites que a IA mandou fazem sentido.
function limitesValidos(l) {
  if (!l || typeof l !== "object") return null;
  const saida = {};
  for (const k in FAIXAS) {
    const par = l[k];
    if (!Array.isArray(par) || par.length !== 2) return null;
    const [min, max] = par.map(Number);
    if (!isFinite(min) || !isFinite(max) || min >= max) return null;
    if (min < FAIXAS[k][0] || max > FAIXAS[k][1]) return null;
    saida[k] = [min, max];
  }
  return saida;
}

// Grow light: horas de luz por dia (0 a 24) e hora do dia em que acende (0 a 23).
function luzValida(l) {
  if (!l || typeof l !== "object") return null;
  const horas = Number(l.horas);
  const inicio = Number(l.inicio);
  if (!isFinite(horas) || horas < 0 || horas > 24) return null;
  if (!Number.isInteger(inicio) || inicio < 0 || inicio > 23) return null;
  return { horas, inicio };
}
const LUZ_PADRAO = { horas: 12, inicio: 6 };

const cache = new Map(); // guarda a resposta de cada planta para ela não mudar a cada busca

// 1) A IA sugere os valores ideais para uma planta.
app.post("/api/parametros", async (req, res) => {
  const planta = String(req.body.planta || "").trim().slice(0, 60);
  if (!planta) return res.status(400).json({ erro: "Digite o nome da planta." });
  const chave = planta.toLowerCase();
  if (cache.has(chave)) return res.json(cache.get(chave));

  const system =
    "Você é um agrônomo especialista em cultivo em estufa. " +
    "Responda SOMENTE com um JSON, sem texto antes ou depois e sem markdown. " +
    'Se a planta for reconhecida: {"limites":{"temp":[min,max],"ar":[min,max],"solo":[min,max],"ph":[min,max]},"luz_horas":numero,"observacao":"uma frase curta"}. ' +
    "temp é a temperatura do ar em °C, ar é a umidade relativa do ar em %, solo é a umidade do solo em %, ph é o pH da água de rega, luz_horas é o total de horas de luz por dia (fotoperíodo, de 0 a 24) com grow light. " +
    'Se não for uma planta cultivável ou você não tiver certeza: {"erro":"motivo curto"}. ' +
    "Use valores para a fase de crescimento vegetativo, típicos para cultivo protegido.";

  try {
    const texto = await perguntarIA(system, [{ role: "user", content: "Planta: " + planta }], 2000, 0);
    const json = JSON.parse(texto.slice(texto.indexOf("{"), texto.lastIndexOf("}") + 1));
    if (json.erro) return res.status(422).json({ erro: json.erro });
    const limites = limitesValidos(json.limites);
    if (!limites) return res.status(422).json({ erro: "A IA mandou valores estranhos. Tente de novo." });
    const horasIA = Number(json.luz_horas);
    const luz_horas = isFinite(horasIA) && horasIA >= 0 && horasIA <= 24 ? horasIA : null;
    const resultado = { limites, luz_horas, observacao: String(json.observacao || "").slice(0, 200) };
    cache.set(chave, resultado);
    res.json(resultado);
  } catch (e) {
    console.error(e);
    res.status(500).json({ erro: "Não consegui falar com a IA. Motivo: " + e.message });
  }
});

// 2) Chat para tirar dúvidas sobre a planta.
app.post("/api/chat", async (req, res) => {
  const mensagem = String(req.body.mensagem || "").trim().slice(0, 1000);
  if (!mensagem) return res.status(400).json({ erro: "Mensagem vazia." });

  const planta = String(req.body.planta || "").slice(0, 60) || "não informada";
  let historico = Array.isArray(req.body.historico) ? req.body.historico : [];
  historico = historico
    .filter((m) => m && (m.role === "user" || m.role === "assistant") && typeof m.content === "string")
    .slice(-8)
    .map((m) => ({ role: m.role, content: m.content.slice(0, 2000) }));
  while (historico.length && historico[0].role !== "user") historico.shift();

  const system =
    "Você é um assistente simpático de plantas e cultivo, que ajuda uma pessoa iniciante que tem uma estufa. " +
    "Responda em português simples, claro e curto. " +
    "Você pode responder QUALQUER pergunta sobre plantas: espécies e variedades, cuidados (rega, luz, solo, adubo, pragas, doenças, poda, colheita), cultivo dentro ou fora de estufa, usos da planta (culinário, cosmético, medicinal) e curiosidades. " +
    "Se a pessoa citar uma planta na pergunta, responda sobre ela, mesmo que seja diferente da planta configurada. " +
    "Só recuse, de forma educada e breve, assuntos que não tenham nenhuma relação com plantas, agricultura ou a estufa. " +
    "Sobre usos medicinais ou cosméticos, dê informação geral e prudente, sem prometer cura, e lembre de testar alergia ou procurar um profissional de saúde quando fizer sentido. " +
    "Use os dados da estufa abaixo SOMENTE quando a pergunta for sobre a estufa, os sensores ou os limites. Nos outros casos, não cite esses números. " +
    "Você só aconselha: não consegue ligar nem desligar equipamentos. " +
    "Se a dúvida envolver risco (produtos químicos, eletricidade), sugira cuidado. Se não tiver certeza, diga isso e não invente. " +
    "Não termine toda resposta com uma pergunta de oferta de ajuda. " +
    "Dados da estufa: planta configurada = " + planta +
    "; limites configurados = " + JSON.stringify(req.body.limites || {}).slice(0, 500) +
    "; grow light = " + JSON.stringify(req.body.luz || {}).slice(0, 100) +
    "; leituras atuais (podem ser simuladas em testes) = " + JSON.stringify(req.body.leituras || {}).slice(0, 500) + ".";

  try {
    const resposta = await perguntarIA(system, [...historico, { role: "user", content: mensagem }], 2000);
    res.json({ resposta });
  } catch (e) {
    console.error(e);
    res.status(500).json({ erro: "Não consegui falar com a IA. Motivo: " + e.message });
  }
});

// ---------- Banco de dados (Upstash Redis, ligado pelo Vercel) ----------
const R_URL = process.env.KV_REST_API_URL || process.env.UPSTASH_REDIS_REST_URL;
const R_TOKEN = process.env.KV_REST_API_TOKEN || process.env.UPSTASH_REDIS_REST_TOKEN;

async function redis(comando) {
  if (!R_URL || !R_TOKEN) throw new Error("Banco de dados não configurado.");
  const r = await fetch(R_URL, {
    method: "POST",
    headers: { Authorization: "Bearer " + R_TOKEN, "content-type": "application/json" },
    body: JSON.stringify(comando),
  });
  const d = await r.json();
  if (!r.ok || d.error) throw new Error(d.error || "Erro no banco de dados");
  return d.result;
}
async function lerJSON(chave) {
  const t = await redis(["GET", chave]);
  return t ? JSON.parse(t) : null;
}
async function gravarJSON(chave, obj) {
  await redis(["SET", chave, JSON.stringify(obj)]);
}

// Faixas aceitas para as leituras dos sensores (valores fora disso são recusados).
const FAIXAS_LEITURA = { temp: [-20, 80], ar: [0, 100], solo: [0, 100], ph: [0, 14] };
const ATUADORES = ["aquecimento", "ventilacao", "umidificador", "irrigacao", "growlight"];

// 3) ESP32 envia as leituras dos sensores.
app.post("/api/leitura", async (req, res) => {
  const leitura = {};
  for (const k in FAIXAS_LEITURA) {
    const n = Number(req.body[k]);
    if (!isFinite(n) || n < FAIXAS_LEITURA[k][0] || n > FAIXAS_LEITURA[k][1]) {
      return res.status(400).json({ erro: "Valor inválido em " + k });
    }
    leitura[k] = n;
  }
  const at = req.body.atuadores && typeof req.body.atuadores === "object" ? req.body.atuadores : {};
  leitura.atuadores = {};
  for (const k of ATUADORES) leitura.atuadores[k] = at[k] === true;
  leitura.ts = Date.now();
  try {
    await gravarJSON("estufa:leitura", leitura);
    res.json({ ok: true });
  } catch (e) {
    console.error(e);
    res.status(500).json({ erro: "Não consegui salvar a leitura." });
  }
});

// 4) Site busca a última leitura.
app.get("/api/leitura", async (req, res) => {
  try {
    const leitura = await lerJSON("estufa:leitura");
    if (!leitura) return res.json({ leitura: null });
    res.json({ leitura, idade_s: Math.round((Date.now() - leitura.ts) / 1000) });
  } catch (e) {
    console.error(e);
    res.status(500).json({ erro: "Não consegui ler a leitura." });
  }
});

// 5) ESP32 pergunta quais são os limites atuais.
app.get("/api/config", async (req, res) => {
  try {
    const config = await lerJSON("estufa:config");
    res.json(config || { limites: null, versao: 0 });
  } catch (e) {
    console.error(e);
    res.status(500).json({ erro: "Não consegui ler os limites." });
  }
});

// 6) Site salva novos limites (exige senha configurada, pois mexe na estufa).
app.post("/api/config", async (req, res) => {
  if (!SENHA) {
    return res.status(403).json({ erro: "Defina SENHA_SITE para poder alterar os limites da estufa." });
  }
  const limites = limitesValidos(req.body.limites);
  if (!limites) return res.status(422).json({ erro: "Limites inválidos ou fora da faixa segura." });
  const luz = luzValida(req.body.luz);
  if (!luz) return res.status(422).json({ erro: "Exposição à grow light inválida (horas de 0 a 24, início de 0 a 23)." });
  try {
    const config = { limites, luz, versao: Date.now() };
    await gravarJSON("estufa:config", config);
    res.json({ ok: true, versao: config.versao });
  } catch (e) {
    console.error(e);
    res.status(500).json({ erro: "Não consegui salvar os limites." });
  }
});


// 7) Site lê os limites que estão aplicados na estufa.
app.get("/api/limites", async (req, res) => {
  try {
    res.json((await lerJSON("estufa:config")) || { limites: null, versao: 0 });
  } catch (e) {
    console.error(e);
    res.status(500).json({ erro: "Não consegui ler os limites." });
  }
});

if (require.main === module) {
  app.listen(PORTA, () => console.log("Estufa no ar: http://localhost:" + PORTA));
}
module.exports = app; // o Vercel usa isto
