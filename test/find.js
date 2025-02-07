// This is just used to debug the package while you develop things

var audioDetect = require('..');

console.log('startMonitoring');
audioDetect.startMonitoring();

//1 find
audioDetect.find()
	.then(function(devices) {
		console.log(new Date(), 'find', devices.length, devices);
	});

//2 add
audioDetect.on('add', function(device) {
	console.log(new Date(), 'add', device);
});

//2 remove
audioDetect.on('remove', function(device) {
	console.log(new Date(), 'remove', device);
});
