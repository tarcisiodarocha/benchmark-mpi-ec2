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

## Parâmetros do comando mpirun

| Parâmetro | Explicação |
|---|---|
| `--mca plm_rsh_no_tree_spawn 1` | Desativa o lançamento SSH em árvore no Open MPI 4.1.x: o controle inicia os processos remotos, dispensando SSH de um worker para outro. |
| `--hostfile hosts-1` | Seleciona o arquivo com os workers permitidos e seus slots. Use `hosts-2` ou `hosts-3` nas outras configurações. |
| `-np 2` | Define o total de processos MPI: 2, 4 ou 6 neste exemplo. |
| `--map-by ppr:2:node` | Distribui dois processos por nó de trabalho; `ppr` significa processos por recurso e `node` é o recurso escolhido. |
| `--rank-by slot` | Numera os ranks consecutivamente nos slots de cada nó; com este mapeamento, o worker 1 recebe os ranks 0 e 1. |
| `--bind-to none` | Não fixa os processos em CPUs específicas; permite que o sistema operacional os escalone. |
| `--nooversubscribe` | Impede iniciar mais processos que os slots disponíveis nos nós. |
| `--wdir /home/ubuntu/mpi-bilateral` | Define o diretório de trabalho dos processos nos workers. Esse diretório deve existir. |

Depois dessas opções vem o executável `/home/ubuntu/mpi-bilateral/bilateral`. Os argumentos seguintes são recebidos pelo **programa C**:

| Argumento | Explicação |
|---|---|
| `12` | Raio da vizinhança do filtro: até 12 pixels em cada direção, formando uma janela de 25 × 25. |
| `6` | Sigma espacial: controla quanto o peso diminui com a distância entre pixels. |
| `30` | Sigma de intensidade: controla quanto o peso diminui com a diferença entre os tons de cinza. |
| `/home/ubuntu/mpi-bilateral/saida-1` | Pasta onde o rank 0 grava as imagens filtradas; varia para `saida-2` ou `saida-3` conforme a execução. |
| Caminhos terminados em `01.pgm`, `06.pgm` e `12.pgm` | Imagens de entrada que compõem o pacote a processar. |

As opções `-p` e `-o tempo-N-r1.txt`, antes de `mpirun`, pertencem ao `/usr/bin/time`: selecionam o formato das medidas e o arquivo onde serão salvas.
