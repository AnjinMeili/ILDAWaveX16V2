// UF2 is a binary block format; parse it directly rather than scraping tool output.
const fs = require('node:fs');

function requireValid(condition, message) {
    if (!condition) {
        throw new Error(message);
    }
}

const [filename, originText, sizeText] = process.argv.slice(2);
const origin = Number(originText);
const flashSize = Number(sizeText);
requireValid(Number.isSafeInteger(origin) && Number.isSafeInteger(flashSize) && flashSize > 0, 'Invalid linked flash bounds');

const data = fs.readFileSync(filename);
requireValid(data.length > 0 && data.length % 512 === 0, 'Invalid UF2 file size');

const application = [];
const compatibility = [];

for (let offset = 0; offset < data.length; offset += 512) {
    const block = data.subarray(offset, offset + 512);
    const word = index => block.readUInt32LE(index * 4);
    requireValid(word(0) === 0x0a324655 && word(1) === 0x9e5d5157 && word(127) === 0x0ab16f30, 'Invalid UF2 magic');

    const flags = word(2);
    const address = word(3);
    const size = word(4);
    const number = word(5);
    const count = word(6);
    const family = word(7);
    requireValid(size === 256 && address % 256 === 0, 'Unexpected UF2 payload layout');

    if (family === 0xe48bff57) {
        // RP2350-E10 compatibility blocks are not application flash data.
        requireValid((flags === 0x2000 || flags === 0xa000) && number === 0 && count === 2, 'Invalid compatibility block');
        requireValid(block.subarray(32, 288).every(byte => byte === 0xef), 'Invalid compatibility payload');

        if (flags === 0xa000) {
            requireValid(block.readUInt32LE(288) === 0x9957e304, 'Invalid compatibility extension');
        }

        compatibility.push({address, family, flags});
        continue;
    }

    requireValid(family === 0xe48bff59 && flags === 0x2000, 'Expected RP2350 Arm secure application family');
    requireValid(address >= origin && address + size <= origin + flashSize, 'Application block exceeds linked flash bounds');
    application.push({address, end: address + size, number, count});
}

requireValid(application.length > 0 && compatibility.length <= 1, 'Unexpected UF2 block groups');

for (let index = 0; index < application.length; index++) {
    const block = application[index];
    requireValid(block.number === index && block.count === application.length, 'Invalid application block numbering');

    if (index > 0) {
        requireValid(block.address >= application[index - 1].end, 'Overlapping or unordered application blocks');
    }
}

console.log(JSON.stringify({
    linkedFlash: {origin, size: flashSize},
    application: {
        family: '0xe48bff59',
        blocks: application.length,
        start: application[0].address,
        endExclusive: application[application.length - 1].end
    },
    compatibilityBlocks: compatibility,
    installationApproved: false,
    note: 'Linked pico2 geometry is not validation of RP2354 physical flash or settings reservations.'
}, null, 2));
