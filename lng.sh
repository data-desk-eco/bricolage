#!/bin/bash
# one lng pipeline run over every exporting coast: terminals, then the companies behind each.
# run detached on the jump host: (nohup ./lng.sh > run.log 2>&1 &)
cd "$(dirname "$0")"
. ~/data-desk/infra/env
export BRIC_KEY=$DEEPSEEK_API_KEY BRIC_MODEL=deepseek-flash BRIC_URL=https://api.deepseek.com/anthropic/v1/messages BRIC_WORKERS=6
sqlite3 lng.db < lng.sql
sqlite3 lng.db -cmd ".load ./ext/bric" "insert into coast values
  ('United States Gulf Coast'), ('Alaska'), ('Canada'), ('Mexico'), ('Trinidad and Tobago'), ('Peru'), ('Argentina'),
  ('Qatar'), ('Oman'), ('United Arab Emirates'), ('Yemen'), ('Egypt'), ('Algeria'), ('Libya'),
  ('Nigeria'), ('Equatorial Guinea'), ('Cameroon'), ('Gabon'), ('Republic of the Congo'), ('Angola'), ('Mozambique'), ('Tanzania'), ('Senegal and Mauritania'),
  ('Norway'), ('Russia'), ('Australia'), ('Papua New Guinea'), ('Indonesia'), ('Malaysia'), ('Brunei'), ('Vietnam')"
