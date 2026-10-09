#pragma once

namespace eruption {

// ERUPTION_TEST_BC7_ALPHA=1: roda um teste sintetico de ida-e-volta (comprime
// e decodifica) do MESMO compressor/parametros que EmbeddedTextureBake usa
// pra' empacotar o canal de altura (MRAH-W, BcKind::Data) sobre padroes
// CONHECIDOS (rampa, ruido, xadrez), e loga min/max/media/desvio antes e
// depois. Existe pra' responder "o BC7 esta' lavando o contraste da altura?"
// com numero, em segundos, sem carregar mapa nenhum e sem GPU/Vulkan (o
// compressor e o decodificador sao os dois so' CPU) - nasceu de uma sessao
// de debug onde a resposta acabou sendo NAO (ver o comentario em
// Render.cpp, m_postSettings.pbrDebugActive: o culpado real era o
// tonemap/exposicao rodando por cima da cor de auditoria, nao o codec).
//
// Chame o mais cedo possivel em main() - antes de GpuAutoSelect/Engine::init.
// Devolve true quando o teste rodou (o chamador deve sair sem continuar o
// boot normal), false quando a env var nao estava setada.
bool runBcAlphaSelfTestIfRequested();

} // namespace eruption
