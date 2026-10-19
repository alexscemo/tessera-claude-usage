# Uso di Claude Code

Una tile con i limiti del tuo piano Claude Code, gli stessi numeri di `/usage` in Claude Code:
- quanto hai usato della sessione di 5 ore, e quando si azzera;
- quanto hai usato della settimana, e quando si azzera.

Toccando la tile si apre una scheda con tutti i limiti, compresa la settimana di un modello (Opus o Sonnet) sui piani
che ce l'hanno. La scheda mostra anche il giorno e l'ora di ogni azzeramento, e quando sono arrivati gli ultimi numeri.

La tile legge **un sensore di Home Assistant**. Un piccolo script sul computer dove usi Claude Code scrive quel sensore
ogni 5 minuti. Né lo schermo né l'app Tessera parlano con Anthropic.

## Configurazione

1. Sul computer dove usi Claude Code, con Python 3.9 o più recente, copia `tools/push_usage.ini.example` in
   `tools/push_usage.ini`. Inserisci l'indirizzo di Home Assistant e un token di accesso di lunga durata. Per crearlo, in
   Home Assistant apri il tuo profilo, poi Sicurezza, poi Token di accesso di lunga durata, poi Crea token.
2. Esegui `python tools/push_usage.py --print`. Mostra i numeri che invierebbe, che devono corrispondere a `/usage` in
   Claude Code.
3. Esegui una volta `python tools/push_usage.py`. In Home Assistant, Strumenti per sviluppatori > Stati ora mostra
   `sensor.claude_code_usage`.
4. Su Windows esegui `python tools/push_usage.py --install-task`, così l'Utilità di pianificazione invia i numeri ogni 5
   minuti. Su macOS o Linux esegui lo script ogni 5 minuti con cron.
5. In Tessera aggiungi il plugin a uno schermo. Lo schermo viene ricompilato una volta.
6. In Layout posiziona la tile **Uso di Claude Code** dal gruppo Plugin della libreria (da 1x1 fino a 2x2) e scegli il
   sensore nell'ispettore.

## Da sapere

- **Dimensioni della tile:**
  - Una tile 1x1 mostra la sessione.
  - Una tile più larga mostra sessione e settimana affiancate.
  - Una tile più alta le mostra una sopra l'altra, sotto il nome della tile.
- **Colori:** la barra è arancione caldo, ambra dal 75% e rossa dal 90%; la percentuale diventa rossa dal 90%.
- **Clawd:** la mascotte di Claude Code sta accanto alla sessione e sbatte gli occhi ogni pochi secondi. Cammina sul
  posto mentre usi Claude Code (la sessione è salita rispetto ai numeri precedenti), e diventa grigio quando i numeri
  sono vecchi.
- **Azzeramenti:** passata l'ora di azzeramento, la tile conta subito quel limite come 0%, senza aspettare i numeri
  successivi.
- **Quando i numeri non si possono leggere:** se il login di Claude Code è scaduto, o Anthropic non risponde, il
  sensore tiene gli ultimi numeri e dice che sono vecchi. Il login si rinnova la prossima volta che usi Claude Code. Lo
  script non lo rinnova mai da solo, perché disconnetterebbe Claude Code. Anche i numeri vecchi di più di 15 minuti
  vengono mostrati come vecchi, per esempio quando il computer è spento o in sospensione.
- **Da dove vengono i numeri:** dall'indirizzo che Claude Code stesso usa per `/usage`. Anthropic non lo documenta,
  quindi una nuova versione di Claude Code potrebbe cambiarlo. In quel caso lo script scrive il problema in
  `tools/push_usage.log`.
- **Dopo un riavvio di Home Assistant:** un sensore scritto in questo modo sparisce fino all'invio successivo, al
  massimo 5 minuti dopo.
- **Cosa condivide:** lo script legge il login di Claude Code da `~/.claude/.credentials.json`, lo usa solo per
  chiedere l'uso e non lo invia mai a Home Assistant.
- **Provato finora:** solo sul banco dei plugin (`tools/preview.py`). Non è ancora stato provato su uno schermo vero.
