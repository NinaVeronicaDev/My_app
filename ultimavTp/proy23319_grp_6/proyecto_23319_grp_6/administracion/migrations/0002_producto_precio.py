# Generated by Django 3.2.18 on 2023-05-20 21:52

from django.db import migrations, models


class Migration(migrations.Migration):

    dependencies = [
        ('administracion', '0001_initial'),
    ]

    operations = [
        migrations.AddField(
            model_name='producto',
            name='precio',
            field=models.PositiveIntegerField(null=True, verbose_name='Precio'),
        ),
    ]
