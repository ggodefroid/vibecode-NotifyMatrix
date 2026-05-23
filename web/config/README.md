# Configuration web NotifyMatrix

## Mode 1 — Première mise sous tension (recommandé)

Au **premier démarrage** (ou après effacement de la config), le panneau crée un réseau Wi‑Fi :

| Paramètre | Valeur |
|-----------|--------|
| SSID | `NotifyMatrix-Setup` |
| Mot de passe | `notifymatrix` |
| Page web | http://192.168.4.1 |

1. Connectez votre téléphone ou PC à ce Wi‑Fi.
2. Ouvrez http://192.168.4.1 (redirection automatique / portail captif).
3. Remplissez le formulaire (Wi‑Fi, MQTT, PRIM, lignes).
4. Cliquez sur **Enregistrer sur le panneau** — le panneau redémarre et rejoint votre box.

La configuration est stockée en **mémoire flash (NVS)** sur l’ESP32.

## Accès via l’IP du panneau (réseau local)

Une fois configuré et connecté à votre box, ouvrez dans un navigateur :

`http://<IP-du-panneau>/`

(L’IP est affichée dans le moniteur série au démarrage : `[web] config UI http://192.168.x.x/`)

Vous pouvez modifier la config à tout moment sans repasser par le Wi‑Fi `NotifyMatrix-Setup`.

Pour reconfigurer depuis zéro : effacez la partition NVS (`pio run -t erase`) ou re-flashez après reset usine.

## Mode 2 — Fichier `.env` (développement PC)

La même page `index.html` peut générer un fichier `.env` pour la compilation PlatformIO :

1. Ouvrez `index.html` sur un PC (hors mode panneau).
2. Téléchargez `.env` et placez-le à la racine du projet.
3. `pio run -e matrixportal_s3 -t upload`

Les valeurs `.env` servent de **valeurs par défaut** dans le formulaire du portail si aucune config NVS n’existe encore.

## Import

Bouton **Importer .env** pour pré-remplir le formulaire depuis un fichier existant.
