# Éclairage LED RGB 12V piloté par ESP32

## Câblage
- Alimentation 12V (celle en 2A que tu as déjà) → fil commun +12V du ruban, et **masse (12V-) reliée au GND de l'ESP32**
- Fil R du ruban → Drain MOSFET #1 (IRLZ44N) → Source → GND commun
- Fil G du ruban → Drain MOSFET #2 → Source → GND commun
- Fil B du ruban → Drain MOSFET #3 → Source → GND commun
- Gate MOSFET #1 → GPIO 25 (via résistance ~150 ohm) + résistance 10k entre gate et source
- Gate MOSFET #2 → GPIO 26 (idem)
- Gate MOSFET #3 → GPIO 27 (idem)
- ESP32 alimenté séparément en 5V (USB ou buck 12V→5V)

Les numéros de GPIO (25/26/27) sont modifiables en haut de `src/main.cpp` si tu préfères d'autres broches.

## Configuration wifi (sans identifiants en dur)
Au premier démarrage (ou si la connexion échoue), l'ESP32 émet son propre
réseau wifi **"Lumière"** (mot de passe **"123456"**) :
1. Connecte-toi à ce réseau depuis ton téléphone/ordinateur
2. Une page de configuration doit s'ouvrir automatiquement (portail captif) ;
   sinon va sur `http://192.168.4.1`
3. Saisis le SSID et le mot de passe de ta box, valide
4. L'ESP32 enregistre les identifiants en mémoire flash (NVS) et redémarre
5. Reconnecte ton appareil à ta box habituelle, puis va sur `http://eclairage.local`
6. Une fois connecté à la box, le réseau "Lumière" disparaît automatiquement
   (il ne réapparaît que si la connexion à la box échoue au démarrage)

⚠️ "123456" ne fait que 6 caractères : en dessous du minimum WPA2 (8 caractères),
l'ESP32 crée en réalité un réseau **ouvert**, sans mot de passe. Modifie
`AP_PASS` en haut de `main.cpp` si tu veux une vraie protection.

Le fuseau horaire est réglé sur Paris (`CET-1CEST,M3.5.0,M10.5.0/3`) dans
`main.cpp`, à adapter si besoin.

## Installation avec PlatformIO
```
# depuis le dossier du projet
pio run                     # compile
pio run --target uploadfs   # envoie la page web (data/) vers l'ESP32
pio run --target upload     # envoie le firmware
pio device monitor          # (optionnel) voir les logs, dont l'adresse IP
```

Une fois démarré, la page est accessible depuis n'importe quel appareil connecté
à la même box, via :
- `http://eclairage.local` (si ton réseau supporte mDNS, ce qui est le cas sur la plupart des box/routeurs récents)
- ou directement par IP (affichée dans les logs série au démarrage)

## Fonctionnement des modes
- **Fixe** : couleur 1 fixe
- **Breathe** : couleur 1, luminosité qui pulse doucement
- **Fade** : transition douce en boucle entre couleur 1 et couleur 2
- **Wave** : comme le ruban n'est pas adressable (toute sa longueur s'allume
  en même temps), "wave" est ici une rotation continue de teinte dans le
  temps plutôt qu'un déplacement physique de lumière le long du ruban

## Mode réveil
- Active/désactive, heure, jours de la semaine et durée de montée réglables dans l'UI
- À l'heure programmée, la lumière monte progressivement en "blanc chaud"
  (RGB 255/200/120) sur la durée choisie
- Le dernier réglage (mode/couleurs/luminosité/config réveil) est sauvegardé
  en mémoire flash (NVS) et restauré automatiquement après une coupure de courant

## Pistes d'amélioration futures
- OTA (mise à jour du firmware à distance, sans câble)
- Ajout d'un vrai bouton "extinction progressive" (coucher) symétrique au réveil
- Historique / statistiques d'utilisation
- Bouton "oublier le wifi" dans l'UI pour relancer le provisioning sans devoir couper la box
