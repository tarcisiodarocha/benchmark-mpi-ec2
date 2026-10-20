# benchmark-mpi-ec2

Processamento de imagens em C + Open MPI, com execuções manuais e medição pelo time do Linux.

O exemplo usa as **quatro EC2 já configuradas no [guia do cluster](https://github.com/tarcisiodarocha/mpi-ec2)**: uma de controle com code-server e três workers. O processamento é escrito somente em C com Open MPI. Você executa manualmente cada configuração com `mpirun` e mede com o `time` do Linux. Não há scripts de benchmark nem dependência de Python.

Execute os comandos abaixo no **terminal do code-server, na instância de controle**, como usuário comum. Clone este repositório no controle e entre na pasta do benchmark:

```bash
git clone https://github.com/tarcisiodarocha/benchmark-mpi-ec2.git
cd benchmark-mpi-ec2
```

## O processamento útil

O programa `bilateral.c` aplica um **filtro bilateral** a um pacote de imagens. Ele reduz ruído e suaviza pequenas variações preservando contornos: combina proximidade espacial com semelhança de intensidade entre os pixels.

O pacote contém três mapas reais da coleção gratuita [NASA Blue Marble: Next Generation](https://science.nasa.gov/earth/earth-observatory/blue-marble-next-generation/base-map/), referentes a janeiro, junho e dezembro de 2004. Crédito: NASA Earth Observatory / Reto Stöckli. As [diretrizes da NASA](https://www.nasa.gov/nasa-brand-center/images-and-media/) permitem uso educacional/informativo com reconhecimento da origem.

Para visualizar a restauração, os comandos de preparação acrescentam ruído às cópias em cinza e preservam as referências. **As cenas são reais; o ruído é adicionado para a demonstração.** O filtro é uma implementação direta, apropriada para estudar paralelismo; não pretende substituir bibliotecas otimizadas de imagens.

## 1. Preparar o pacote no controle

Open MPI e o compilador já devem estar instalados. Para baixar/converter as imagens e medir o tempo, instale somente estas ferramentas no controle:

```bash
sudo apt-get update
sudo apt-get install -y curl imagemagick time
mkdir -p originais referencia entrada resultados
```

Baixe as três imagens:

```bash
curl -fL --retry 3 -o originais/01.jpg \
  https://assets.science.nasa.gov/content/dam/science/esd/eo/images/bmng/bmng-base/january/world.200401.3x5400x2700.jpg
curl -fL --retry 3 -o originais/06.jpg \
  https://assets.science.nasa.gov/content/dam/science/esd/eo/images/bmng/bmng-base/june/world.200406.3x5400x2700.jpg
curl -fL --retry 3 -o originais/12.jpg \
  https://assets.science.nasa.gov/content/dam/science/esd/eo/images/bmng/bmng-base/december/world.200412.3x5400x2700.jpg
```

Converta para cinza, com resolução de 2048 × 1024, e acrescente ruído reproduzível nesta instalação do ImageMagick:

```bash
for mes in 01 06 12; do
    convert "originais/$mes.jpg" -resize 2048x1024! \
        -colorspace Gray -depth 8 +compress "referencia/$mes.pgm" || break
    convert "referencia/$mes.pgm" -seed 2026 -attenuate 0.15 +noise Gaussian \
        -colorspace Gray -depth 8 +compress "entrada/$mes.pgm" || break
done
```

`convert` é o comando do ImageMagick 6 disponível no Ubuntu 24.04. Em instalações ImageMagick 7, use `magick` no lugar de `convert`. O formato de saída deve ser **PGM binário P5, com maxval 255**. `+compress` seleciona a representação binária; não use `-compress none`, que seleciona a representação textual dos formatos PNM.

Confira as dimensões e a primeira linha de cada entrada:

```bash
identify entrada/*.pgm
head -n 1 entrada/01.pgm
head -n 1 entrada/06.pgm
head -n 1 entrada/12.pgm
```

A primeira linha deve ser `P5`. Para processar as imagens sem adicionar ruído, use `cp referencia/*.pgm entrada/` antes de copiá-las para os workers. Download e conversão são preparação; ficam fora da medição de desempenho MPI.

## 2. Compilar o programa C

```bash
mpicc -O3 -std=c11 -Wall -Wextra -Werror bilateral.c -lm -o bilateral
```

A interface do programa é:

```text
bilateral RAIO SIGMA_ESPACIAL SIGMA_INTENSIDADE PASTA_SAIDA ENTRADA1.pgm [ENTRADA2.pgm ...]
```

Neste experimento usamos `12 6 30`. Um raio 12 define uma vizinhança de 25 × 25 pixels. O pacote tem 6.291.456 pixels e aproximadamente 3,9 bilhões de avaliações de vizinhança, descontadas as bordas. Não há pausas nem trabalho artificial para aumentar a duração.

## 3. Copiar o binário e os dados para os workers

Os nomes e o SSH são os mesmos do guia anterior. Este roteiro pressupõe o usuário `ubuntu` e o caminho `/home/ubuntu/mpi-bilateral` nos três workers:

```bash
for host in mpi-worker-1 mpi-worker-2 mpi-worker-3; do
    ssh "$host" 'mkdir -p /home/ubuntu/mpi-bilateral/entrada /home/ubuntu/mpi-bilateral/saida-1 /home/ubuntu/mpi-bilateral/saida-2 /home/ubuntu/mpi-bilateral/saida-3' || break
    scp bilateral "$host":/home/ubuntu/mpi-bilateral/bilateral || break
    scp entrada/01.pgm entrada/06.pgm entrada/12.pgm \
        "$host":/home/ubuntu/mpi-bilateral/entrada/ || break
done
```

Só prossiga se todas as cópias terminarem sem erro. As três máquinas precisam usar a mesma arquitetura, bibliotecas compatíveis e a mesma versão Open MPI 4.1.x do controle, conforme o guia existente. Não é necessário NFS. Cada execução usa arquivos previamente copiados.

## 4. Entender as variações

Os três hostfiles já estão nesta pasta:

| Arquivo | Workers incluídos | Processos MPI |
|---|---|---:|
| `hosts-1` | `mpi-worker-1` | 2 |
| `hosts-2` | `mpi-worker-1`, `mpi-worker-2` | 4 |
| `hosts-3` | Os três workers | 6 |

Cada linha declara `slots=2`. Usamos dois processos por instância, seguindo o guia das `t3.small`. O controle **não consta nos hostfiles**, portanto não executa o filtro. O rank 0 fica no worker 1, que também processa sua faixa de linhas.

A comparação mantém **o mesmo pacote, a mesma resolução e os mesmos parâmetros**, acrescentando workers. Trata-se de escalabilidade forte. A referência é uma instância com dois processos; não equivale a comparar com execução serial de um processo.

As opções comuns são:

- `--map-by ppr:2:node`: dois processos por worker.
- `--rank-by slot`: o primeiro worker recebe o rank 0.
- `--bind-to none`: mantém a configuração do guia para instâncias cujas duas vCPUs podem compartilhar um núcleo físico.
- `--nooversubscribe`: impede exceder os slots declarados.
- `--mca plm_rsh_no_tree_spawn 1`: mantém o lançamento SSH centralizado do **Open MPI 4.1.x**, conforme a configuração existente.

## 5. Executar manualmente e medir com time

Use `/usr/bin/time` para selecionar explicitamente o GNU time, disponível pelo pacote `time`. `-p` imprime `real`, `user` e `sys`; `-o` salva essas medidas separadamente da saída do MPI.

### Uma instância de trabalho

```bash
/usr/bin/time -p -o tempo-1-r1.txt \
mpirun --mca plm_rsh_no_tree_spawn 1 \
    --hostfile hosts-1 -np 2 \
    --map-by ppr:2:node --rank-by slot --bind-to none --nooversubscribe \
    --wdir /home/ubuntu/mpi-bilateral \
    /home/ubuntu/mpi-bilateral/bilateral 12 6 30 \
    /home/ubuntu/mpi-bilateral/saida-1 \
    /home/ubuntu/mpi-bilateral/entrada/01.pgm \
    /home/ubuntu/mpi-bilateral/entrada/06.pgm \
    /home/ubuntu/mpi-bilateral/entrada/12.pgm
```

### Duas instâncias de trabalho

```bash
/usr/bin/time -p -o tempo-2-r1.txt \
mpirun --mca plm_rsh_no_tree_spawn 1 \
    --hostfile hosts-2 -np 4 \
    --map-by ppr:2:node --rank-by slot --bind-to none --nooversubscribe \
    --wdir /home/ubuntu/mpi-bilateral \
    /home/ubuntu/mpi-bilateral/bilateral 12 6 30 \
    /home/ubuntu/mpi-bilateral/saida-2 \
    /home/ubuntu/mpi-bilateral/entrada/01.pgm \
    /home/ubuntu/mpi-bilateral/entrada/06.pgm \
    /home/ubuntu/mpi-bilateral/entrada/12.pgm
```

### Três instâncias de trabalho

```bash
/usr/bin/time -p -o tempo-3-r1.txt \
mpirun --mca plm_rsh_no_tree_spawn 1 \
    --hostfile hosts-3 -np 6 \
    --map-by ppr:2:node --rank-by slot --bind-to none --nooversubscribe \
    --wdir /home/ubuntu/mpi-bilateral \
    /home/ubuntu/mpi-bilateral/bilateral 12 6 30 \
    /home/ubuntu/mpi-bilateral/saida-3 \
    /home/ubuntu/mpi-bilateral/entrada/01.pgm \
    /home/ubuntu/mpi-bilateral/entrada/06.pgm \
    /home/ubuntu/mpi-bilateral/entrada/12.pgm
```

Cada comando deve terminar sem erro e informar três imagens processadas. A saída de diagnóstico identifica o host de cada rank. Se houver erro, descarte essa medição.

Veja os tempos:

```bash
cat tempo-1-r1.txt tempo-2-r1.txt tempo-3-r1.txt
```

**Compare o campo `real`, em segundos:** ele mede o tempo decorrido para todo o `mpirun`, incluindo lançamento SSH/MPI, leitura, distribuição dos dados, filtro, reunião, gravação e finalização. Não inclui download, conversão, compilação e cópias feitas antes/depois.

`user` e `sys` descrevem o consumo de CPU observado pelo comando no controle e seus descendentes locais. **Não representam a soma de CPU consumida nos workers remotos.** Por isso, o ganho entre instâncias deve ser avaliado pelo tempo `real`.

Para ver o tempo diretamente no terminal, em vez de salvá-lo, basta remover `-o tempo-N-r1.txt`. Também pode usar a palavra-chave do Bash `time -p mpirun ...`; o caminho `/usr/bin/time` facilita salvar as medidas com `-o`.

## 6. Repetir e calcular o ganho manualmente

Faça primeiro uma execução de cada configuração para aquecimento e descarte os tempos. Depois repita **manualmente** cada configuração pelo menos três vezes, de preferência cinco, mudando os nomes dos arquivos para `r1`, `r2`, `r3` etc. Isso evita sobrescrever resultados. Alterne a ordem das configurações entre rodadas: 1/2/3, depois 2/3/1, depois 3/1/2.

Anote os valores `real` e use a mediana: ordene os três ou cinco tempos de cada configuração e escolha o valor central.

| Workers | Processos | Mediana de real (s) | Aceleração | Eficiência |
|---:|---:|---|---|---|
| 1 | 2 | T1 | 1 | 100% |
| 2 | 4 | T2 | T1 / T2 | 100 × T1 / (2 × T2) |
| 3 | 6 | T3 | T1 / T3 | 100 × T1 / (3 × T3) |

Por exemplo, **se** você medir 60, 33 e 24 segundos, as acelerações serão 1,00×, 1,82× e 2,50×. Esses valores são apenas um exemplo de cálculo, não resultados medidos nas EC2.

Se a execução com um worker durar poucos segundos, aumente a resolução na etapa 1 para `4096x2048!`, gere novamente as entradas e copie-as para os três workers. Depois refaça **todas** as configurações com essa mesma carga. Não aumente só a carga da execução com três workers.

Instâncias T3 usam créditos de CPU. Observe o saldo `CPUCreditBalance`, o modo Standard/Unlimited e a carga concorrente para evitar comparar máquinas em condições diferentes. Créditos e custos estão descritos no [guia da AWS](https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/burstable-credits-baseline-concepts.html). Ganho linear não é garantido: comunicação, disco e lançamento também consomem tempo.

Para comparar um processo por instância, altere **nos três comandos** `ppr:2:node` para `ppr:1:node` e `-np` para 1, 2 e 3. Mantenha o mesmo número de processos por worker em toda a comparação. Os hostfiles podem continuar declarando dois slots; slots são capacidade, não processos obrigatórios.

## 7. Conferir que o resultado é o mesmo

Depois de executar as três configurações, calcule hashes no worker 1, onde estão as três saídas:

```bash
ssh mpi-worker-1 'cd /home/ubuntu/mpi-bilateral/saida-1 && sha256sum 01.pgm 06.pgm 12.pgm' > hashes-1.txt
ssh mpi-worker-1 'cd /home/ubuntu/mpi-bilateral/saida-2 && sha256sum 01.pgm 06.pgm 12.pgm' > hashes-2.txt
ssh mpi-worker-1 'cd /home/ubuntu/mpi-bilateral/saida-3 && sha256sum 01.pgm 06.pgm 12.pgm' > hashes-3.txt
diff -u hashes-1.txt hashes-2.txt
diff -u hashes-1.txt hashes-3.txt
```

Confira que os três comandos SSH terminaram sem erro e que cada arquivo contém três linhas. Os dois `diff` devem terminar sem diferenças. A divisão em mais processos deve mudar o tempo, mantendo os mesmos pixels de saída.

Para visualizar o resultado, recupere as imagens e converta uma comparação para PNG:

```bash
scp mpi-worker-1:/home/ubuntu/mpi-bilateral/saida-3/01.pgm resultados/01.pgm
scp mpi-worker-1:/home/ubuntu/mpi-bilateral/saida-3/06.pgm resultados/06.pgm
scp mpi-worker-1:/home/ubuntu/mpi-bilateral/saida-3/12.pgm resultados/12.pgm
convert referencia/01.pgm entrada/01.pgm resultados/01.pgm \
    +append resultados/comparacao-01.png
```

Abra o PNG pelo code-server. As três imagens aparecem na ordem: referência, entrada com ruído e saída filtrada. A suavização depende dos sigmas e pode reduzir detalhes; mantenha os parâmetros fixos na comparação de desempenho.

## Como ler o código C

O lote é processado uma imagem por vez. Para cada imagem, o rank 0 lê o PGM e envia dimensões e pixels por `MPI_Bcast`. Cada processo calcula uma faixa de linhas, usando a imagem completa para acessar vizinhos. `MPI_Gatherv` reúne as faixas no rank 0, que grava o resultado. A memória dessa imagem é liberada antes da próxima.

A divisão aceita alturas não divisíveis pelo total de processos e até faixas vazias. A replicação dos pixels simplifica o exemplo; para imagens enormes, uma evolução seria distribuir apenas as faixas e seus halos. Não há threads adicionais: o paralelismo é entre processos MPI.

Use nomes de arquivo distintos no lote e crie as pastas de saída antes de executar, pois o programa preserva o nome de cada entrada. O C aceita PGM P5 de 8 bits, maxval 255. JPEGs são convertidos previamente no controle.

O C foi compilado com avisos tratados como erros, e a correção do filtro já foi conferida localmente com 1, 2, 3 e 6 processos e uma referência independente. A medição de desempenho deve ser feita nas EC2 usando os comandos acima.

Referências: [GNU time](https://www.gnu.org/software/time/manual/time.html), [opções do mpirun](https://www.open-mpi.org/doc/v4.0/man1/mpirun.1.php) e [formatos ImageMagick/PNM](https://imagemagick.org/script/formats.php).
