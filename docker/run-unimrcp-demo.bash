#!/bin/bash -ex

SPEECH_CONTAINER_NAME="${SPEECH_CONTAINER_NAME:-localsr}"
SPEECH_CONTAINER_PORT="${SPEECH_CONTAINER_PORT:-5000}"
SPEECH_CR="${SPEECH_CR:-containerpreview.azurecr.io}"
SPEECH_REPO="${SPEECH_REPO:-microsoft/cognitive-services-speech-to-text}"
SPEECH_TAG="${SPEECH_TAG:-1.2.0-amd64-en-us-preview}"
SPEECH_REGION="${SPEECH_REGION:-invalid}"
SPEECH_APIKEY="${SPEECH_APIKEY:-invalid}"
SPEECH_MAX_DECODERS="${SPEECH_MAX_DECODERS:-20}"
UNIMRCP_CR="${UNIMRCP_CR:-skyman.azurecr.io}"
UNIMRCP_REPO="${UNIMRCP_REPO:-scratch/onprem/unimrcp}"
UNIMRCP_CLIENT_TAG="${UNIMRCP_CLIENT_TAG:-asrclient}"
UNIMRCP_SERVER_TAG="${UNIMRCP_SERVER_TAG:-server}"
SPEECH_SELECT="${SPEECH_SELECT:-local}"

# parsing command line arguments:
while [[ $# > 0 ]]; do
    key="$1"
    case $key in
        -h|--help)
            echo "Usage: run-unimrcp-demo [run_options]"
            echo "Options:"
            echo "  -r|--region <region> - speech region to use"
            echo "  -k|--apikey <key> - api key to use"
            echo "  -c|--cloud - select cloud instead of local container"
            exit 1
            ;;
        -r|--region)
            SPEECH_REGION="$2"
            shift
            ;;
        -k|--apikey)
            SPEECH_APIKEY="$2"
            shift
            ;;
        -c|--cloud)
            SPEECH_SELECT="cloud"
            ;;
        *)
            echo "Unknown option $key -- Ignoring"
            ;;
    esac
    shift # past argument or value ($1)
done

# Computed values
SOURCE_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
SPEECH_BILLING="https://${SPEECH_REGION}.api.cognitive.microsoft.com"
SPEECH_ENDPOINT="${SPEECH_CONTAINER_NAME}:${SPEECH_CONTAINER_PORT}"
SPEECH_DCR="${SPEECH_CR}/${SPEECH_REPO}:${SPEECH_TAG}"
UNIMRCP_SERVER_DCR="${UNIMRCP_CR}/${UNIMRCP_REPO}:${UNIMRCP_SERVER_TAG}"
UNIMRCP_CLIENT_DCR="${UNIMRCP_CR}/${UNIMRCP_REPO}:${UNIMRCP_CLIENT_TAG}"
sed -e "s/YourSpeechContainerEndPoint/$SPEECH_ENDPOINT/g" -e "s/YourSubscriptionKey/$SPEECH_APIKEY/g" -e "s/YourServiceRegion/$SPEECH_REGION/g"  ${SOURCE_DIR}/config.json > ${SOURCE_DIR}/config.json.rej

if [[ "${SPEECH_SELECT}" == "cloud" ]]; then
    sed -i "s/sr_use_local_container\"\:\strue/sr_use_local_container\": false/g" ${SOURCE_DIR}/config.json.rej
fi

# Make sure mandatory arguments are passed properly
if [[ "${SPEECH_APIKEY}" == "invalid" ]]; then
    echo "Must supply speech api key!"
    exit 1
fi

if [[ "${SPEECH_REGION}" == "invalid" ]]; then
    echo "Must supply speech region!"
    exit 1
fi

# Run docker images
DOCKER_LINK=""
if [[ "${SPEECH_SELECT}" != "cloud" ]]; then
    docker kill ${SPEECH_CONTAINER_NAME} || echo "No speech container running, that is OK"
    docker pull ${SPEECH_DCR}
    docker run --rm -d --name ${SPEECH_CONTAINER_NAME} -e DECODER_MAX_COUNT=${SPEECH_MAX_DECODERS} -p ${SPEECH_CONTAINER_PORT}:5000 "${SPEECH_DCR}" eula=accept billing=${SPEECH_BILLING} apikey=${SPEECH_APIKEY}
    DOCKER_LINK="--link ${SPEECH_CONTAINER_NAME}:${SPEECH_CONTAINER_NAME}"
fi

# Cleanup previous instances of the containers
docker kill unimrcpserver asrclient || echo "No UniMRCP containers running, that is OK"

docker pull ${UNIMRCP_SERVER_DCR}
docker pull ${UNIMRCP_CLIENT_DCR}
docker run --rm -d --name unimrcpserver ${DOCKER_LINK} -v ${SOURCE_DIR}/config.json.rej:/usr/local/unimrcp/conf/config.json -p 8060:8060/udp -p 8060:8060/tcp -p 1544:1544 -p 1554:1554 "${UNIMRCP_SERVER_DCR}"

echo "RUN THE FOLLOWING COMMAND> run grammar.xml whatstheweatherlike.wav"
docker run --rm -ti --name asrclient --net=host "${UNIMRCP_CLIENT_DCR}"

