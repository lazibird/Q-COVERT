const OpenAI = require("openai");
const { fetch, ProxyAgent } = require("undici");

function createDeepSeekClient() {
  const proxyUrl = process.env.HTTPS_PROXY || process.env.https_proxy;
  const dispatcher = proxyUrl ? new ProxyAgent(proxyUrl) : null;

  return new OpenAI({
    baseURL: "https://api.deepseek.com/v1",
    apiKey: process.env.DEEPSEEK_API_KEY ?? null,
    ...(dispatcher && {
      fetch: (url, init) => fetch(url, { ...init, dispatcher }),
    }),
  });
}

module.exports = { createDeepSeekClient };
