# Éditeur des programmations horaires (v0.8, expérimental)

> **v1.0.0 :** document historique. Les options d'autorisation (`enable_*`,
> `allow_write`, `validated_days`), les entités « État modification / commande »
> et les exemples intermédiaires cités ici n'existent plus. État actuel :
> `docs/v1.0.0.md` et `examples/iot-viessmann-v1.0.0.yaml`.


> **Statut : implémenté et testé en simulation seulement.** L'écriture des
> blocs de programmation des radiateurs A1 (`0x2000`), du plancher M2
> (`0x3000`) et de l'ECS (`0x2100`) n'a **jamais été essayée sur la
> chaudière**. Le bloc du bouclage (`0x2200`) n'est écrit jusqu'ici que par
> le bouclage temporaire. Faire les premiers essais **un circuit et un jour
> à la fois** (§ 8).

## 1. Principe

L'éditeur permet de consulter et de modifier depuis Home Assistant les
programmations horaires de la Vitotronic 300 KW3 :

| Circuit | Clé | Lundi | Dimanche |
|---|---|---|---|
| Radiateurs A1 | `a1` | `0x2000` | `0x2030` |
| Plancher chauffant M2 | `m2` | `0x3000` | `0x3030` |
| Production ECS | `ecs` | `0x2100` | `0x2130` |
| Bouclage ECS | `bouclage` | `0x2200` | `0x2230` |

Chaque jour est un bloc de 8 octets : 4 plages `début fin` au plus, octet
horaire = heure × 8 + dizaines de minutes, `FF FF` = plage libre
(`components/visslink/schedule.h`).

**La Vitotronic reste l'unique source de vérité.**

- La carte n'affiche que ce que l'ESP a lu sur l'Optolink.
- Home Assistant ne garde aucune programmation : pas d'entrée `schedule`,
  pas de copie parallèle.
- Une modification faite au panneau de la Vitotronic est détectée. Elle
  n'est **jamais écrasée** silencieusement.
- « Enregistré » n'est affiché qu'après la **relecture exacte des 8 octets**
  écrits.

## 2. Architecture

```
 Carte Lovelace visslink-schedule-card.js        Scripts / automatisations
        │ hass.callService("esphome", ..., return_response)       │
        ▼                                                         ▼
 Actions ESPHome (api: actions:)  visslink_schedule_read / set / copy /
                                   clear / refresh / undo
        │ lambdas id(programmations).read(...), set_days(...), ...
        ▼
 visslink_schedule_editor ── séquenceur ScheduleEditor (C++ pur, testé sur PC)
        │  un jour = un bloc = un job : lecture, contrôle, sauvegarde,
        │  écriture, relecture
        ▼
 visslink : arbitre unique des écritures, lectures ponctuelles, liste
            blanche par client, ordonnanceur (inchangés)
        ▲
 visslink_boost (bouclage temporaire) ── verrou mutuel avec l'éditeur
```

- **Entités standard ESPHome.** Une seule nouvelle entité : « Programmations
  - État de l'édition » (text_sensor). Les capteurs de programmation
  existants (résumé, jour, plage active) ne changent pas.
- **Actions Home Assistant.** Elles sont déclarées dans le YAML : c'est
  l'interface publique et stable d'ESPHome, sans option `custom_services`.
- **Carte Lovelace personnalisée.** Un seul fichier JavaScript, sans
  dépendance ni compilation. Elle n'utilise que l'objet `hass` public
  (`callService` avec réponse, états), sans aucun élément interne `ha-*`.
  Elle reste donc compatible avec les mises à jour du frontend.

ESPHome ne fournit **pas** d'éditeur de programmation hebdomadaire. Cette
carte le remplace.

## 3. Configuration ESPHome

Exemple complet : `examples/iot-viessmann-programmations-v0.8.yaml`.

1. Ajouter `visslink_schedule_editor` aux composants externes :

   ```yaml
   external_components:
     - source: github://ubikyo/visslink
       components: [visslink, visslink_boost, visslink_schedule_editor]
   ```

2. Donner un `id` aux programmations existantes. Aucune lecture n'est
   ajoutée :

   ```yaml
   text_sensor:
     - platform: visslink
       id: programmation_a1
       name: "Radiateurs - Programmation"
       address: 0x2000
       ...
   ```

3. Déclarer l'éditeur :

   ```yaml
   visslink_schedule_editor:
     id: programmations          # nom utilisé par les actions
     visslink_id: viessmann
     schedules:                  # 1 à 4 programmations visslink
       - programmation_a1
       - programmation_m2
       - programmation_ecs
       - programmation_bouclage
     status:
       name: "Programmations - État de l'édition"
   ```

4. Déclarer les six actions sous `api:`. Les recopier telles quelles depuis
   l'exemple : la carte utilise ces noms.

   ```yaml
   api:
     encryption:
       key: !secret api_encryption_key
     actions:
       - action: visslink_schedule_read
         variables:
           circuit: string
         then:
           - api.respond:
               data: !lambda "id(programmations).read(circuit, root);"
       - action: visslink_schedule_set
         variables:
           circuit: string
           days: string
           ranges: string
           expected: string
         then:
           - api.respond:
               success: !lambda "return id(programmations).set_days(circuit, days, ranges, expected);"
               error_message: !lambda "return id(programmations).last_error();"
       # visslink_schedule_copy, _clear, _refresh, _undo : voir l'exemple
   ```

Quelques remarques sur cette configuration :

- **Autorisations.** Aucune option n'est à ajouter : l'écriture des
  programmations suit `enable_writes` et `enable_schedule_writes` du
  composant `visslink`, actives par défaut depuis la v0.7.2. Avec
  `enable_writes: false`, l'éditeur reste consultable mais refuse toute
  modification.
- **Validation.** `schedules` n'accepte que les text_sensor `visslink` de
  type programmation aux adresses `0x2000`, `0x3000`, `0x2100` et `0x2200`,
  chacun une seule fois.
- **Format des jours.** Les jours sont passés en texte (`"0,1,2"`). En effet,
  `api.respond` d'ESPHome 2026.10 ne compile pas avec une variable `int[]`.

## 4. Actions Home Assistant

Dans Home Assistant, chaque action est préfixée par le nom du nœud ESPHome,
tirets remplacés par `_`. Exemple : `esphome.iot_viessmann_visslink_schedule_set`.

| Action | Variables | Effet |
|---|---|---|
| `visslink_schedule_read` | `circuit` | Réponse JSON : 7 jours (plages, octets lus, âge de la lecture, verrou du bouclage), opération en cours ou dernière, annulation possible. Ne lit rien sur l'Optolink. |
| `visslink_schedule_set` | `circuit`, `days`, `ranges`, `expected` | Écrit les mêmes plages sur chaque jour listé. |
| `visslink_schedule_copy` | `circuit`, `source`, `days`, `expected` | Copie les 8 octets lus du jour `source` vers les jours listés. |
| `visslink_schedule_clear` | `circuit`, `days`, `expected` | Efface les plages des jours listés. |
| `visslink_schedule_refresh` | `circuit` | Relit les 7 jours depuis la Vitotronic : environ 3 min, un jour toutes les 30 s. |
| `visslink_schedule_undo` | — | Réécrit le bloc d'origine de la dernière modification, seulement si le bloc lu est encore celui qui a été écrit. |

Valeurs des variables :

- **`circuit`** : `a1`, `m2`, `ecs` ou `bouclage`. L'adresse du lundi
  (`0x2000`, etc.) est aussi acceptée.
- **`days`** : `"0,1,2"`, de 0 (lundi) à 6 (dimanche), ou `"*"` pour les sept
  jours.
- **`ranges`** : `"06:00-08:30,17:00-22:00"`. `""` signifie aucune plage.
  Règles :
  - pas de 10 minutes ;
  - 4 plages au plus ;
  - fin au plus tard à 23:50 ;
  - pas de chevauchement. Des plages contiguës (`06:00-08:00,08:00-09:00`)
    sont admises.

  Les plages sont triées puis écrites compactées.
- **`expected`** : le champ `expected` renvoyé par `visslink_schedule_read`,
  c'est-à-dire les blocs affichés à l'utilisateur. `""` signifie la dernière
  lecture de l'ESP.

Une demande refusée fait échouer l'action avec le motif, par exemple « Radiateurs
Me modifié depuis l'affichage (panneau ?) : programmation rechargée,
recommencer ». Une demande acceptée renvoie un succès immédiat. Le résultat
arrive ensuite dans l'entité d'état et dans `visslink_schedule_read`.

Exemple de script (lundi à vendredi, plancher) :

```yaml
action: esphome.iot_viessmann_visslink_schedule_set
data:
  circuit: m2
  days: "0,1,2,3,4"
  ranges: "05:30-08:00,16:30-22:30"
  expected: ""
```

## 5. Carte Lovelace : installation

1. **Versions.** Les réponses des actions (`api.respond`) existent depuis
   ESPHome 2025.12 ; le firmware est compilé avec ESPHome 2026.10. Côté
   Home Assistant, l'intégration ESPHome doit enregistrer ces actions avec
   réponse : `visslink_schedule_read` en `optional` (données JSON si
   `return_response`), les cinq autres en `status`, vues par Home Assistant
   comme des actions sans réponse qui échouent avec le motif du refus.
   Utiliser une version de Home Assistant au moins aussi récente que
   l'ESPHome du firmware. Si la lecture échoue avec un message du type
   « does not support responses », mettre Home Assistant à jour.
2. **Copier le fichier.** Copier `lovelace/visslink-schedule-card.js` dans
   le dossier de configuration de Home Assistant :

   ```
   /config/www/visslink/visslink-schedule-card.js
   ```

   Le dossier `www` est servi à l'adresse `/local/`. Si `www` vient d'être
   créé, redémarrer Home Assistant une fois.
3. **Déclarer la ressource.** Aller dans Paramètres → Tableaux de bord → ⋮
   (en haut à droite) → Ressources → « Ajouter une ressource ». Le menu
   Ressources n'apparaît qu'avec le « mode avancé » activé dans le profil
   utilisateur. Renseigner :
   - URL : `/local/visslink/visslink-schedule-card.js?v=0.8.0`
   - Type : « Module JavaScript »

   Pour un tableau de bord en mode YAML, l'équivalent est :

   ```yaml
   lovelace:
     resources:
       - url: /local/visslink/visslink-schedule-card.js?v=0.8.0
         type: module
   ```

4. **Trouver les deux noms.** Il en faut deux :
   - **`device`** : le nom du nœud ESPHome (`esphome: name:`), tirets
     remplacés par `_`. Exemple : `iot-viessmann` donne `iot_viessmann`. Il
     se vérifie dans Outils de développement → Actions, en cherchant
     `visslink_schedule_read`.
   - **`status_entity`** : l'entité « Programmations - État de l'édition ».
     Elle se trouve dans Paramètres → Appareils → l'appareil ESPHome. Exemple :
     `sensor.chaudiere_viessmann_programmations_etat_de_l_edition`.
5. **Ajouter la carte.** Modifier le tableau de bord → Ajouter une carte →
   « Manuel » :

   ```yaml
   type: custom:visslink-schedule-card
   device: iot_viessmann
   status_entity: sensor.chaudiere_viessmann_programmations_etat_de_l_edition
   title: Programmations          # facultatif
   circuits: [a1, m2, ecs, bouclage]   # facultatif : ordre et filtre
   ```

6. **Après une mise à jour de la carte.** Remplacer le fichier, changer le
   paramètre `?v=` de la ressource (par exemple `?v=0.8.1`), puis recharger
   la page en vidant le cache. Sur l'application mobile : Paramètres →
   Compagnon → Débogage → Réinitialiser le cache du frontend.

Pour **voir la carte sans Home Assistant**, ouvrir
`lovelace/visslink-schedule-card-preview.html` dans un navigateur. La page
utilise des données fictives et simule les écritures, en vue ordinateur et
téléphone.

## 6. Utilisation

Maquette, sur ordinateur ou tablette :

```
┌ Programmations ──────────────── données lues il y a 2 h 05   ⟳ ┐
│ [Radiateurs] [Plancher] [ECS] [Bouclage ECS]                    │
│       0     6     12    18    24                                │
│ Lu  ▕      ████████████████████▏ 06:00-22:00  enregistré (relu)│
│ Ma  ▕      ████████████████████▏ 06:00-22:00  écriture en cours│
│ Me  ▕      ████████████████████▏ 06:00-22:00  en attente       │
│ …                                                               │
│ Di  ▕       ███████████████████▏ 07:00-23:00                    │
├─────────────────────────────────────────────────────────────────┤
│ Lundi  [Modifications non enregistrées]  lu il y a 2 h 05       │
│  [05:30] → [08:00]  ✕                                           │
│  [16:30] → [22:30]  ✕                                           │
│  + Ajouter une plage (2 libres)                                 │
│ Appliquer aussi à : ☐Ma ☐Me ☐Je ☐Ve ☐Sa ☐Di  Lu→Ve  Tous  Aucun │
│        [Effacer la journée] [Abandonner] [Enregistrer (3 jours)]│
│ Radiateurs : écriture Ma (2/3) · enregistrés : Lu               │
└─────────────────────────────────────────────────────────────────┘
```

Sur téléphone, la grille garde la largeur de l'écran et les boutons passent
à la ligne. Les zones tactiles font au moins 36 px.

Pour modifier une programmation :

1. Choisir le circuit (onglets), puis le jour (ligne de la grille).
2. Modifier les plages : heures au pas de 10 min, `✕` pour supprimer,
   « + Ajouter une plage » (4 au plus).
3. Facultatif : cocher d'autres jours dans « Appliquer aussi à », ou utiliser
   « Lu→Ve » ou « Tous ». « Effacer la journée » vide les plages ; il faut
   ensuite « Enregistrer ».
4. Cliquer sur « Enregistrer ». Sans modification du jour, avec des jours
   cochés, c'est une **copie exacte** des octets lus.

États affichés :

| Affichage | Signification |
|---|---|
| « Modifications non enregistrées », « brouillon » | Rien n'est encore envoyé. Changer de jour ou de circuit demande « Enregistrer » ou « Abandonner ». |
| « en attente », « écriture en cours » | Jour accepté par l'ESP, pas encore relu. |
| « enregistré (relu) » | Les 8 octets relus sont exactement ceux qui ont été écrits. |
| « inchangé » | Le jour avait déjà ces plages : rien n'a été écrit. |
| « échec », « non enregistré » | Le jour a échoué (motif dans la ligne d'état). Les jours suivants n'ont pas été traités. |
| « bouclage temporaire » | Plage temporaire du bouclage en cours : jour non modifiable. |
| « non lu » (hachuré) | Jour pas encore lu depuis le démarrage de l'ESP : le rafraîchir. |

L'**âge des données** est affiché pour la semaine (lecture la plus ancienne)
et pour le jour choisi. Le bouton ⟳ relit les 7 jours.

« Annuler la dernière modification » apparaît tant que la sauvegarde existe.
L'annulation est refusée si le jour a été modifié depuis.

## 7. Déroulement et protections

Pour chaque jour modifié (un « job »), l'ESP enchaîne :

1. Il lit les 8 octets du jour (lecture ponctuelle prioritaire).
2. Il les décode strictement. Un bloc incohérent entraîne un refus, et rien
   n'est écrit.
3. Il compare ce bloc à celui que l'utilisateur avait sous les yeux
   (`expected`). En cas de différence, c'est un refus : « modifié depuis la
   Vitotronic ». L'affichage est alors resynchronisé, et la modification du
   panneau est conservée.
4. Si le bloc est déjà identique au nouveau, le jour est « inchangé » et
   rien n'est écrit.
5. Il sauvegarde en flash l'ancien et le nouveau bloc. Si la flash échoue,
   rien n'est écrit.
6. Il écrit les 8 octets par l'arbitre unique : une écriture à la fois,
   10 s au moins entre deux.
7. Il relit et compare exactement. Le jour n'est enregistré qu'à ce
   moment-là, et l'affichage reprend le bloc relu.

Autres règles :

- **Un jour à la fois.** Une copie vers 5 jours fait 5 jobs, confirmés un par
  un, et jamais les sept jours pour un seul. Les jours déjà identiques ne
  sont pas réécrits.
- **Premier échec.** L'opération s'arrête. Le message liste les jours
  enregistrés, le jour en échec avec son motif, et les jours non traités.
- **Liaison perdue après l'écriture.** L'issue est indéterminée. L'ESP
  relit toutes les 30 s pendant 15 min, sans jamais réécrire. Au-delà :
  « issue indéterminée : vérifier au panneau ».
- **Redémarrage de l'ESP.** La dernière écriture est seulement vérifiée par
  une lecture : confirmée, non appliquée, ou remplacée depuis. Aucune
  écriture n'a lieu au démarrage.
- **Listes blanches par client.** L'éditeur ne peut écrire que les 28 blocs
  des quatre programmations. Le bouclage reste limité aux 7 blocs `0x2200`.
  Aucun autre registre n'est accessible à l'éditeur.
- **Bouclage temporaire.**
  - Pendant toute l'opération du bouclage (plage active, restauration,
    récupération), toute modification de programmation est refusée
    explicitement. Le jour concerné est signalé dans la carte.
  - Réciproquement, le bouclage refuse de démarrer pendant une modification.
  - La restauration du bouclage n'est donc jamais concurrencée. Elle ne
    restaure que si le bloc lu est encore le bloc temporaire : une
    modification faite entre-temps n'est jamais écrasée par une sauvegarde
    ancienne.
  - Le bouclage 10/20/30 min est inchangé.
- **Charge de la liaison.** L'éditeur n'ajoute aucune lecture périodique.
  Les 28 blocs restent lus toutes les 6 h, 30 s entre deux blocs
  (`BLOCK_DELAY_MS` inchangé). Une modification coûte 1 lecture,
  1 écriture et 1 relecture par jour. Le rafraîchissement manuel relance
  le cycle d'un seul circuit.
- **Synchronisation avec le panneau.** Une modification faite à la
  Vitotronic apparaît au cycle suivant, au rafraîchissement manuel, ou dès
  qu'une modification de ce jour est tentée, puisque la lecture préalable
  la détecte et l'affiche.
- **Usure de la flash.** Il y a une sauvegarde par jour écrit. Les
  modifications restent manuelles et rares.

## 8. Premiers essais sur la chaudière

Ne rien automatiser avant ces essais. Procéder un circuit et un jour à la
fois, en commençant par un jour sans enjeu, par exemple le bouclage un jour
de semaine.

1. Noter au panneau la programmation du jour choisi, et relever son bloc
   dans les journaux DEBUG (`0x2208 <- 31 43 AC AD FF FF FF FF`).
2. Dans la carte, ajouter ou décaler une plage de 10 min sur ce seul jour,
   puis « Enregistrer ».
3. Attendre « Enregistré et relu ». Vérifier dans les journaux la trame
   `0x2208 -> ... (schedule block write)` puis la relecture identique.
4. Vérifier **au panneau** que le bon jour a changé, et lui seul.
5. Revenir en arrière avec « Annuler la dernière modification ». Vérifier
   de nouveau au panneau.
6. Répéter pour un jour du week-end, puis pour chaque circuit (A1, M2, ECS),
   toujours un jour à la fois.

Arrêter et signaler toute différence, par exemple un autre jour modifié ou
un affichage du panneau différent de la relecture. Dans ce cas, revenir en
lecture seule avec `enable_schedule_writes: false` sous `visslink:`.

## 9. Limites connues

- Fin de plage limitée à 23:50 : 24:00 (`0xC0`) n'a jamais été observé sur
  cette KW3. Une copie d'un jour existant recopie toutefois ses octets tels
  quels.
- La correspondance bloc ↔ jour n'a été vérifiée au panneau que pour le
  bouclage, un dimanche (`docs/bouclage-ecs-v0.5.md`).
- Le verrou avec le bouclage est global : aucune modification pendant les
  10 à 30 min d'un bouclage temporaire, même sur un autre circuit.
- Les jours sont passés en texte dans les actions (limite d'`api.respond`
  dans ESPHome 2026.10).

## 10. Dépannage

| Symptôme | Cause probable |
|---|---|
| « Aucune donnée » dans la carte | `device` incorrect, ESP hors ligne, ou Home Assistant trop ancien pour les réponses d'actions |
| Custom element doesn't exist: visslink-schedule-card | Ressource absente ou mauvaise URL ; vider le cache |
| « Refusé : … pas encore lu » | Jour non lu depuis le démarrage de l'ESP : ⟳, puis attendre environ 3 min |
| « Refusé : … modifié depuis l'affichage » | Changement fait au panneau : la carte s'est rechargée, recommencer |
| « Refusé : bouclage ECS temporaire en cours » | Attendre la fin de la restauration du bouclage |
| « Écritures de programmation désactivées » | `enable_writes: false` ou `enable_schedule_writes: false` sous `visslink:` |
